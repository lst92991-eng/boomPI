package qwenpipeline

import (
	"context"
	"crypto/rand"
	"encoding/base64"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"net/http"
	"strings"
	"time"
	"unicode"

	"github.com/gorilla/websocket"
)

// DashScope counts CJK characters as two units and other characters as one.
// Keep some headroom below the provider's 2,000-unit fragment limit.
const (
	maxTTSFragmentUnits         = 1600
	maxTTSPCMDeltaBytes         = 64 * 1024
	maxTTSWebSocketMessageBytes = 128 * 1024
	ttsCommitMaxUnits           = 80
)

const ttsCommitIdle = 120 * time.Millisecond

type ttsClient struct {
	config Config
	dialer websocket.Dialer
}

func newTTSClient(config Config) *ttsClient {
	dialer := *websocket.DefaultDialer
	dialer.HandshakeTimeout = config.Timeout
	return &ttsClient{config: config, dialer: dialer}
}

func (c *ttsClient) synthesize(ctx context.Context, text string, emit func([]byte) error) error {
	fragments := splitTTSFragments(text, maxTTSFragmentUnits)
	if len(fragments) == 0 {
		return errors.New("Qwen TTS input text is empty")
	}
	stream := make(chan string, len(fragments))
	for _, fragment := range fragments {
		stream <- fragment
	}
	close(stream)
	return c.synthesizeStream(ctx, stream, emit)
}

func (c *ttsClient) synthesizeStream(
	ctx context.Context,
	fragments <-chan string,
	emit func([]byte) error,
) error {
	header := make(http.Header)
	header.Set("Authorization", "Bearer "+c.config.APIKey)
	header.Set("X-DashScope-WorkSpace", c.config.WorkspaceID)
	connection, response, err := c.dialer.DialContext(ctx, c.config.ttsURL(), header)
	if response != nil && response.Body != nil {
		response.Body.Close()
	}
	if err != nil {
		return fmt.Errorf("connect Qwen TTS: %w", err)
	}
	defer connection.Close()
	watchDone := make(chan struct{})
	go func() {
		select {
		case <-ctx.Done():
			_ = connection.Close()
		case <-watchDone:
		}
	}()
	defer close(watchDone)
	return c.synthesizeConnectedStream(ctx, connection, fragments, emit)
}

func (c *ttsClient) synthesizeConnectedStream(
	ctx context.Context,
	connection *websocket.Conn,
	fragments <-chan string,
	emit func([]byte) error,
) error {
	connection.SetReadLimit(maxTTSWebSocketMessageBytes)
	if err := c.waitFor(ctx, connection, "session.created", nil); err != nil {
		return err
	}
	update := map[string]any{
		"event_id": eventID(),
		"type":     "session.update",
		"session": map[string]any{
			"voice": c.config.TTSVoice, "mode": "commit",
			"language_type": "Chinese", "response_format": "pcm",
			"sample_rate": 24000,
		},
	}
	if err := c.writeJSON(connection, update); err != nil {
		return err
	}
	if err := c.waitFor(ctx, connection, "session.updated", nil); err != nil {
		return err
	}

	streamCtx, cancelStream := context.WithCancel(ctx)
	defer cancelStream()
	writerDone := make(chan error, 1)
	go func() {
		err := c.writeContinuousText(streamCtx, connection, fragments)
		writerDone <- err
		if err != nil {
			_ = connection.Close()
		}
	}()

	sawCompletedResponse := false
	audioBytes := 0
	for {
		var event struct {
			Type  string `json:"type"`
			Delta string `json:"delta"`
			Error struct {
				Code    string `json:"code"`
				Message string `json:"message"`
			} `json:"error"`
			Response struct {
				Status string `json:"status"`
			} `json:"response"`
		}
		if err := c.readJSON(ctx, connection, &event); err != nil {
			select {
			case writerErr := <-writerDone:
				if writerErr != nil {
					return writerErr
				}
			default:
			}
			return err
		}
		switch event.Type {
		case "response.audio.delta":
			pcm, err := base64.StdEncoding.DecodeString(event.Delta)
			if err != nil || len(pcm) == 0 || len(pcm)%2 != 0 || len(pcm) > maxTTSPCMDeltaBytes {
				return errors.New("Qwen TTS returned invalid PCM")
			}
			if err := emit(pcm); err != nil {
				return err
			}
			audioBytes += len(pcm)
		case "response.done":
			if event.Response.Status != "completed" {
				return fmt.Errorf("Qwen TTS response status is %q", event.Response.Status)
			}
			sawCompletedResponse = true
		case "session.finished":
			select {
			case err := <-writerDone:
				if err != nil {
					return err
				}
			case <-ctx.Done():
				return ctx.Err()
			}
			if !sawCompletedResponse {
				return errors.New("Qwen TTS session finished without a completed response")
			}
			if audioBytes == 0 {
				return errors.New("Qwen TTS session finished without PCM audio")
			}
			return nil
		case "error":
			return fmt.Errorf("Qwen TTS error code=%q message=%q", event.Error.Code, event.Error.Message)
		}
	}
}

func (c *ttsClient) writeContinuousText(
	ctx context.Context,
	connection *websocket.Conn,
	fragments <-chan string,
) error {
	synthesized := false
	var pending strings.Builder
	pendingUnits := 0
	timer := time.NewTimer(time.Hour)
	if !timer.Stop() {
		<-timer.C
	}
	defer timer.Stop()
	var timerC <-chan time.Time
	armTimer := func() {
		if !timer.Stop() {
			select {
			case <-timer.C:
			default:
			}
		}
		timer.Reset(ttsCommitIdle)
		timerC = timer.C
	}
	flush := func() error {
		text := pending.String()
		pending.Reset()
		pendingUnits = 0
		timerC = nil
		if strings.TrimSpace(text) == "" {
			return nil
		}
		if err := c.writeJSON(connection, map[string]any{
			"event_id": eventID(), "type": "input_text_buffer.append", "text": text,
		}); err != nil {
			return err
		}
		if err := c.writeJSON(connection, map[string]any{
			"event_id": eventID(), "type": "input_text_buffer.commit",
		}); err != nil {
			return err
		}
		synthesized = true
		return nil
	}
	for {
		select {
		case <-ctx.Done():
			return ctx.Err()
		case <-timerC:
			if err := flush(); err != nil {
				return err
			}
		case fragment, ok := <-fragments:
			if !ok {
				if err := flush(); err != nil {
					return err
				}
				if !synthesized {
					return errors.New("Qwen TTS input text is empty")
				}
				return c.writeJSON(connection, map[string]any{
					"event_id": eventID(), "type": "session.finish",
				})
			}
			if fragment == "" {
				continue
			}
			pending.WriteString(fragment)
			pendingUnits += ttsTextUnits(fragment)
			if containsTTSSentenceBoundary(fragment) ||
				pendingUnits >= ttsCommitMaxUnits {
				if err := flush(); err != nil {
					return err
				}
			} else {
				armTimer()
			}
		}
	}
}

func containsTTSSentenceBoundary(text string) bool {
	for _, value := range text {
		if isTTSSentenceBoundary(value) {
			return true
		}
	}
	return false
}

func splitTTSFragments(text string, maxUnits int) []string {
	text = strings.TrimSpace(text)
	if text == "" || maxUnits <= 0 {
		return nil
	}

	var fragments []string
	var current strings.Builder
	currentUnits := 0
	appendSegment := func(segment string) {
		for segment != "" {
			segmentUnits := ttsTextUnits(segment)
			if currentUnits > 0 && currentUnits+segmentUnits > maxUnits {
				fragments = append(fragments, current.String())
				current.Reset()
				currentUnits = 0
			}
			if segmentUnits <= maxUnits {
				current.WriteString(segment)
				currentUnits += segmentUnits
				return
			}

			prefix, remainder := splitTTSHard(segment, maxUnits)
			fragments = append(fragments, prefix)
			segment = remainder
		}
	}

	start := 0
	for index, r := range text {
		if isTTSSentenceBoundary(r) {
			end := index + len(string(r))
			appendSegment(text[start:end])
			start = end
		}
	}
	if start < len(text) {
		appendSegment(text[start:])
	}
	if current.Len() > 0 {
		fragments = append(fragments, current.String())
	}

	result := fragments[:0]
	for _, fragment := range fragments {
		if strings.TrimSpace(fragment) != "" {
			result = append(result, fragment)
		}
	}
	return result
}

func splitTTSHard(text string, maxUnits int) (string, string) {
	units := 0
	for index, r := range text {
		next := units + ttsRuneUnits(r)
		if next > maxUnits {
			return text[:index], text[index:]
		}
		units = next
	}
	return text, ""
}

func ttsTextUnits(text string) int {
	units := 0
	for _, r := range text {
		units += ttsRuneUnits(r)
	}
	return units
}

func ttsRuneUnits(r rune) int {
	if r > unicode.MaxASCII {
		return 2
	}
	return 1
}

func isTTSSentenceBoundary(r rune) bool {
	switch r {
	case '\n', '\r', '.', '!', '?', ';', '。', '！', '？', '；':
		return true
	default:
		return false
	}
}

func (c *ttsClient) waitFor(ctx context.Context, connection *websocket.Conn, wanted string, output any) error {
	for {
		var raw json.RawMessage
		if err := c.readJSON(ctx, connection, &raw); err != nil {
			return err
		}
		var envelope struct {
			Type  string `json:"type"`
			Error struct {
				Code    string `json:"code"`
				Message string `json:"message"`
			} `json:"error"`
		}
		if err := json.Unmarshal(raw, &envelope); err != nil {
			return err
		}
		if envelope.Type == "error" {
			return fmt.Errorf("Qwen TTS error code=%q message=%q", envelope.Error.Code, envelope.Error.Message)
		}
		if envelope.Type != wanted {
			continue
		}
		if output != nil {
			return json.Unmarshal(raw, output)
		}
		return nil
	}
}

func (c *ttsClient) writeJSON(connection *websocket.Conn, value any) error {
	if err := connection.SetWriteDeadline(time.Now().Add(c.config.Timeout)); err != nil {
		return err
	}
	return connection.WriteJSON(value)
}

func (c *ttsClient) readJSON(ctx context.Context, connection *websocket.Conn, output any) error {
	if err := connection.SetReadDeadline(time.Now().Add(c.config.Timeout)); err != nil {
		return err
	}
	if err := connection.ReadJSON(output); err != nil {
		if ctx.Err() != nil {
			return ctx.Err()
		}
		return err
	}
	return nil
}

func eventID() string {
	var value [16]byte
	if _, err := rand.Read(value[:]); err != nil {
		return fmt.Sprintf("event-%d", time.Now().UnixNano())
	}
	return "event-" + hex.EncodeToString(value[:])
}
