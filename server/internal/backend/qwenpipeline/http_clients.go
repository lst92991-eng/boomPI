package qwenpipeline

import (
	"bufio"
	"bytes"
	"context"
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
	answer, err := readResponsesStreamWithTimeout(
		ctx, response.Body, c.config.Timeout, onDelta,
	)
	if err != nil {
		return "", fmt.Errorf("Qwen reasoning: %w", err)
	}
	return answer, nil
}

type streamDeltaRequest struct {
	text   string
	result chan error
}

type streamReadResult struct {
	answer string
	err    error
}

type progressReader struct {
	reader   io.Reader
	progress chan<- struct{}
}

func (reader progressReader) Read(buffer []byte) (int, error) {
	count, err := reader.reader.Read(buffer)
	if count != 0 {
		select {
		case reader.progress <- struct{}{}:
		default:
		}
	}
	return count, err
}

// readResponsesStreamWithTimeout distinguishes a healthy long answer from a
// provider connection that delivered headers and then stopped making progress.
// onDelta stays on this goroutine so downstream backpressure pauses, rather
// than falsely triggering, the provider no-progress timer.
func readResponsesStreamWithTimeout(
	ctx context.Context,
	body io.ReadCloser,
	timeout time.Duration,
	onDelta func(string) error,
) (string, error) {
	readCtx, cancelRead := context.WithCancel(ctx)
	defer cancelRead()
	progress := make(chan struct{}, 1)
	deltas := make(chan streamDeltaRequest)
	result := make(chan streamReadResult, 1)
	go func() {
		answer, err := readResponsesStream(progressReader{reader: body, progress: progress}, func(delta string) error {
			request := streamDeltaRequest{text: delta, result: make(chan error, 1)}
			select {
			case deltas <- request:
			case <-readCtx.Done():
				return readCtx.Err()
			}
			select {
			case err := <-request.result:
				return err
			case <-readCtx.Done():
				return readCtx.Err()
			}
		})
		result <- streamReadResult{answer: answer, err: err}
	}()

	timer := time.NewTimer(timeout)
	defer timer.Stop()
	resetProgress := func() {
		if !timer.Stop() {
			select {
			case <-timer.C:
			default:
			}
		}
		timer.Reset(timeout)
	}
	for {
		select {
		case <-ctx.Done():
			cancelRead()
			_ = body.Close()
			return "", ctx.Err()
		case <-progress:
			resetProgress()
		case request := <-deltas:
			var err error
			if onDelta != nil {
				err = onDelta(request.text)
			}
			request.result <- err
			if err != nil {
				cancelRead()
				_ = body.Close()
				return "", err
			}
			resetProgress()
		case read := <-result:
			return read.answer, read.err
		case <-timer.C:
			cancelRead()
			_ = body.Close()
			return "", fmt.Errorf("provider stream made no progress for %s: %w",
				timeout, context.DeadlineExceeded)
		}
	}
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
