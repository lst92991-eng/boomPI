package qwenpipeline

import (
	"bufio"
	"bytes"
	"context"
	"encoding/base64"
	"encoding/binary"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net"
	"net/http"
	"strings"
	"time"
)

const maxProviderResponseBytes = 8 * 1024 * 1024

type chatMessage struct {
	Role    string `json:"role"`
	Content string `json:"content"`
}

type httpClients struct {
	config Config
	client *http.Client
}

func newHTTPClients(config Config) *httpClients {
	dialTimeout := config.Timeout
	if dialTimeout > 10*time.Second {
		dialTimeout = 10 * time.Second
	}
	transport := &http.Transport{
		Proxy:                 http.ProxyFromEnvironment,
		DialContext:           (&net.Dialer{Timeout: dialTimeout, KeepAlive: 30 * time.Second}).DialContext,
		ForceAttemptHTTP2:     true,
		MaxIdleConns:          32,
		MaxIdleConnsPerHost:   8,
		IdleConnTimeout:       90 * time.Second,
		TLSHandshakeTimeout:   dialTimeout,
		ResponseHeaderTimeout: config.Timeout,
		ExpectContinueTimeout: time.Second,
	}
	// Do not put a whole-request timeout on a streaming response. The header
	// deadline bounds first response latency; provider activity and the device
	// watchdog bound stalls while allowing a healthy long answer to continue.
	return &httpClients{config: config, client: &http.Client{Transport: transport}}
}

func (c *httpClients) transcribe(ctx context.Context, pcm []byte) (string, error) {
	requestCtx, cancel := context.WithTimeout(ctx, c.config.Timeout)
	defer cancel()
	wav, err := pcm16MonoWAV(pcm, 16000)
	if err != nil {
		return "", err
	}
	dataURI := "data:audio/wav;base64," + base64.StdEncoding.EncodeToString(wav)
	payload := struct {
		Model      string `json:"model"`
		Messages   []any  `json:"messages"`
		Stream     bool   `json:"stream"`
		ASROptions any    `json:"asr_options"`
	}{
		Model: c.config.ASRModel,
		Messages: []any{map[string]any{
			"role": "user",
			"content": []any{map[string]any{
				"type":        "input_audio",
				"input_audio": map[string]any{"data": dataURI},
			}},
		}},
		Stream: false,
		ASROptions: map[string]any{
			"language": "zh", "enable_itn": true,
		},
	}
	var response struct {
		Choices []struct {
			Message struct {
				Content string `json:"content"`
			} `json:"message"`
		} `json:"choices"`
	}
	if err := c.postJSON(requestCtx, c.config.compatibleBaseURL()+"/chat/completions", payload, &response); err != nil {
		return "", fmt.Errorf("Qwen ASR: %w", err)
	}
	if len(response.Choices) == 0 || strings.TrimSpace(response.Choices[0].Message.Content) == "" {
		return "", errors.New("Qwen ASR returned no transcript")
	}
	return strings.TrimSpace(response.Choices[0].Message.Content), nil
}

func (c *httpClients) complete(ctx context.Context, instructions string, history []chatMessage) (string, error) {
	requestCtx, cancel := context.WithTimeout(ctx, c.config.Timeout)
	defer cancel()
	payload := struct {
		Model        string        `json:"model"`
		Instructions string        `json:"instructions"`
		Input        []chatMessage `json:"input"`
		Reasoning    any           `json:"reasoning"`
		Tools        []any         `json:"tools,omitempty"`
		Store        bool          `json:"store"`
	}{
		Model:        c.config.ReasoningModel,
		Instructions: instructions,
		Input:        history,
		Reasoning:    map[string]any{"effort": c.config.ReasoningEffort},
		Store:        false,
	}
	if c.config.SearchMode == "auto" {
		payload.Tools = []any{map[string]any{"type": "web_search"}}
	}
	var response struct {
		Output []struct {
			Type    string `json:"type"`
			Content []struct {
				Type string `json:"type"`
				Text string `json:"text"`
			} `json:"content"`
		} `json:"output"`
	}
	if err := c.postJSON(requestCtx, c.config.compatibleBaseURL()+"/responses", payload, &response); err != nil {
		return "", fmt.Errorf("Qwen reasoning: %w", err)
	}
	var answer strings.Builder
	for _, output := range response.Output {
		if output.Type != "message" {
			continue
		}
		for _, content := range output.Content {
			if content.Type == "output_text" {
				answer.WriteString(content.Text)
			}
		}
	}
	if strings.TrimSpace(answer.String()) == "" {
		return "", errors.New("Qwen reasoning returned no answer text")
	}
	return strings.TrimSpace(answer.String()), nil
}

func (c *httpClients) completeStream(
	ctx context.Context,
	instructions string,
	history []chatMessage,
	onDelta func(string) error,
) (string, error) {
	payload := struct {
		Model        string        `json:"model"`
		Instructions string        `json:"instructions"`
		Input        []chatMessage `json:"input"`
		Reasoning    any           `json:"reasoning"`
		Tools        []any         `json:"tools,omitempty"`
		Store        bool          `json:"store"`
		Stream       bool          `json:"stream"`
	}{
		Model:        c.config.ReasoningModel,
		Instructions: instructions,
		Input:        history,
		Reasoning:    map[string]any{"effort": c.config.ReasoningEffort},
		Store:        false,
		Stream:       true,
	}
	if c.config.SearchMode == "auto" {
		payload.Tools = []any{map[string]any{"type": "web_search"}}
	}
	body, err := json.Marshal(payload)
	if err != nil {
		return "", err
	}
	request, err := http.NewRequestWithContext(
		ctx,
		http.MethodPost,
		c.config.compatibleBaseURL()+"/responses",
		bytes.NewReader(body),
	)
	if err != nil {
		return "", err
	}
	request.Header.Set("Authorization", "Bearer "+c.config.APIKey)
	request.Header.Set("Content-Type", "application/json")
	request.Header.Set("Accept", "text/event-stream")
	response, err := c.client.Do(request)
	if err != nil {
		return "", fmt.Errorf("Qwen reasoning: %w", err)
	}
	defer response.Body.Close()
	if response.StatusCode < 200 || response.StatusCode >= 300 {
		return "", fmt.Errorf("Qwen reasoning: %w", providerHTTPError(response))
	}
	answer, err := readResponsesStream(response.Body, onDelta)
	if err != nil {
		return "", fmt.Errorf("Qwen reasoning: %w", err)
	}
	return answer, nil
}

func readResponsesStream(reader io.Reader, onDelta func(string) error) (string, error) {
	scanner := bufio.NewScanner(io.LimitReader(reader, maxProviderResponseBytes+1))
	scanner.Buffer(make([]byte, 64*1024), maxProviderResponseBytes)
	readBytes := 0
	var answer strings.Builder
	for scanner.Scan() {
		line := scanner.Text()
		readBytes += len(line) + 1
		if readBytes > maxProviderResponseBytes {
			return "", errors.New("provider response exceeded size limit")
		}
		if !strings.HasPrefix(line, "data:") {
			continue
		}
		data := strings.TrimSpace(strings.TrimPrefix(line, "data:"))
		if data == "" || data == "[DONE]" {
			continue
		}
		var event struct {
			Type  string `json:"type"`
			Delta string `json:"delta"`
			Error struct {
				Code    string `json:"code"`
				Message string `json:"message"`
			} `json:"error"`
			Response struct {
				Status string `json:"status"`
				Error  struct {
					Code    string `json:"code"`
					Message string `json:"message"`
				} `json:"error"`
			} `json:"response"`
		}
		if err := json.Unmarshal([]byte(data), &event); err != nil {
			return "", fmt.Errorf("decode stream event: %w", err)
		}
		switch event.Type {
		case "response.output_text.delta":
			if event.Delta == "" {
				continue
			}
			answer.WriteString(event.Delta)
			if onDelta != nil {
				if err := onDelta(event.Delta); err != nil {
					return "", err
				}
			}
		case "response.completed":
			if strings.TrimSpace(answer.String()) == "" {
				return "", errors.New("Qwen reasoning returned no answer text")
			}
			return strings.TrimSpace(answer.String()), nil
		case "response.failed", "response.incomplete":
			return "", fmt.Errorf("response status=%q code=%q message=%q",
				event.Response.Status, event.Response.Error.Code, event.Response.Error.Message)
		case "error":
			return "", fmt.Errorf("provider_code=%q provider_message=%q", event.Error.Code, event.Error.Message)
		}
	}
	if err := scanner.Err(); err != nil {
		return "", err
	}
	return "", errors.New("Qwen reasoning stream ended before response.completed")
}

func providerHTTPError(response *http.Response) error {
	limited := io.LimitReader(response.Body, maxProviderResponseBytes+1)
	encoded, err := io.ReadAll(limited)
	if err != nil {
		return err
	}
	if len(encoded) > maxProviderResponseBytes {
		return errors.New("provider error response exceeded size limit")
	}
	var providerError struct {
		Error struct {
			Code    string `json:"code"`
			Message string `json:"message"`
		} `json:"error"`
	}
	_ = json.Unmarshal(encoded, &providerError)
	return fmt.Errorf("HTTP %d provider_code=%q provider_message=%q",
		response.StatusCode, providerError.Error.Code, providerError.Error.Message)
}

func (c *httpClients) postJSON(ctx context.Context, endpoint string, input, output any) error {
	body, err := json.Marshal(input)
	if err != nil {
		return err
	}
	request, err := http.NewRequestWithContext(ctx, http.MethodPost, endpoint, bytes.NewReader(body))
	if err != nil {
		return err
	}
	request.Header.Set("Authorization", "Bearer "+c.config.APIKey)
	request.Header.Set("Content-Type", "application/json")
	response, err := c.client.Do(request)
	if err != nil {
		return err
	}
	defer response.Body.Close()
	limited := io.LimitReader(response.Body, maxProviderResponseBytes+1)
	encoded, err := io.ReadAll(limited)
	if err != nil {
		return err
	}
	if len(encoded) > maxProviderResponseBytes {
		return errors.New("provider response exceeded size limit")
	}
	if response.StatusCode < 200 || response.StatusCode >= 300 {
		var providerError struct {
			Error struct {
				Code    string `json:"code"`
				Message string `json:"message"`
			} `json:"error"`
		}
		_ = json.Unmarshal(encoded, &providerError)
		return fmt.Errorf("HTTP %d provider_code=%q provider_message=%q", response.StatusCode, providerError.Error.Code, providerError.Error.Message)
	}
	if err := json.Unmarshal(encoded, output); err != nil {
		return fmt.Errorf("decode provider response: %w", err)
	}
	return nil
}

func pcm16MonoWAV(pcm []byte, sampleRate uint32) ([]byte, error) {
	if len(pcm) == 0 || len(pcm)%2 != 0 || sampleRate == 0 {
		return nil, errors.New("PCM must contain whole 16-bit mono samples")
	}
	if uint64(len(pcm)) > uint64(^uint32(0))-36 {
		return nil, errors.New("PCM is too large for a WAV container")
	}
	wav := make([]byte, 44+len(pcm))
	copy(wav[0:4], "RIFF")
	binary.LittleEndian.PutUint32(wav[4:8], uint32(len(pcm)+36))
	copy(wav[8:12], "WAVE")
	copy(wav[12:16], "fmt ")
	binary.LittleEndian.PutUint32(wav[16:20], 16)
	binary.LittleEndian.PutUint16(wav[20:22], 1)
	binary.LittleEndian.PutUint16(wav[22:24], 1)
	binary.LittleEndian.PutUint32(wav[24:28], sampleRate)
	binary.LittleEndian.PutUint32(wav[28:32], sampleRate*2)
	binary.LittleEndian.PutUint16(wav[32:34], 2)
	binary.LittleEndian.PutUint16(wav[34:36], 16)
	copy(wav[36:40], "data")
	binary.LittleEndian.PutUint32(wav[40:44], uint32(len(pcm)))
	copy(wav[44:], pcm)
	return wav, nil
}
