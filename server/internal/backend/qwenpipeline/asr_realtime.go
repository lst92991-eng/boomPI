package qwenpipeline

import (
	"context"
	"encoding/base64"
	"encoding/json"
	"errors"
	"fmt"
	"net/http"
	"net/url"
	"strings"
	"sync"
	"time"

	"github.com/gorilla/websocket"
)

const (
	realtimeASRModelName       = "qwen3-asr-flash-realtime"
	realtimeASRMaxMessageBytes = 256 * 1024
)

const realtimeASRConnectTimeout = 10 * time.Second

// 每轮一个云端连接；Actor 顺序调用 Append，Commit 接续读取最终结果。
// 网络写入服从调用方期限，不再拥有另一份 PCM 队列或 writer goroutine。
type asrRealtimeStream struct {
	config     Config
	connection *websocket.Conn
	finished   bool
	closeOnce  sync.Once
}

type realtimeASREvent struct {
	Type       string `json:"type"`
	Transcript string `json:"transcript"`
	Error      struct {
		Code    string `json:"code"`
		Message string `json:"message"`
	} `json:"error"`
}

func openRealtimeASR(ctx context.Context, config Config) (*asrRealtimeStream, error) {
	return openRealtimeASRAt(ctx, config, config.asrRealtimeURL())
}

func openRealtimeASRAt(ctx context.Context, config Config, endpoint string) (*asrRealtimeStream, error) {
	if ctx == nil {
		return nil, errors.New("context is required")
	}
	if strings.TrimSpace(endpoint) == "" {
		return nil, errors.New("realtime ASR endpoint is required")
	}
	connectTimeout := config.Timeout
	if connectTimeout > realtimeASRConnectTimeout {
		connectTimeout = realtimeASRConnectTimeout
	}
	connectCtx, cancelConnect := context.WithTimeout(ctx, connectTimeout)
	defer cancelConnect()
	dialer := *websocket.DefaultDialer
	dialer.HandshakeTimeout = connectTimeout
	header := make(http.Header)
	header.Set("Authorization", "Bearer "+config.APIKey)
	header.Set("OpenAI-Beta", "realtime=v1")
	if strings.TrimSpace(config.WorkspaceID) != "" {
		header.Set("X-DashScope-WorkSpace", config.WorkspaceID)
	}
	connection, response, err := dialer.DialContext(connectCtx, endpoint, header)
	if response != nil && response.Body != nil {
		response.Body.Close()
	}
	if err != nil {
		return nil, fmt.Errorf("connect Qwen realtime ASR: %w", err)
	}
	handshakeDone := make(chan struct{})
	handshakeWatcherDone := make(chan struct{})
	go func() {
		defer close(handshakeWatcherDone)
		select {
		case <-connectCtx.Done():
			_ = connection.Close()
		case <-handshakeDone:
		}
	}()
	defer func() {
		close(handshakeDone)
		<-handshakeWatcherDone
	}()
	setupConfig := config
	setupConfig.Timeout = connectTimeout
	stream := &asrRealtimeStream{
		config:     setupConfig,
		connection: connection,
	}
	connection.SetReadLimit(realtimeASRMaxMessageBytes)
	if err := stream.waitFor(connectCtx, "session.created"); err != nil {
		stream.Close()
		return nil, err
	}
	update := map[string]any{
		"event_id": eventID(),
		"type":     "session.update",
		"session": map[string]any{
			"modalities":         []string{"text"},
			"input_audio_format": "pcm",
			"sample_rate":        16000,
			"input_audio_transcription": map[string]any{
				"language": "zh",
			},
			"turn_detection": nil,
		},
	}
	if err := stream.writeJSON(connectCtx, update); err != nil {
		stream.Close()
		return nil, err
	}
	if err := stream.waitFor(connectCtx, "session.updated"); err != nil {
		stream.Close()
		return nil, err
	}
	stream.config = config
	return stream, nil
}

func (c Config) asrRealtimeURL() string {
	if strings.TrimSpace(c.WorkspaceID) == "" {
		return fmt.Sprintf("wss://dashscope.aliyuncs.com/api-ws/v1/realtime?model=%s",
			url.QueryEscape(c.ASRModel))
	}
	return fmt.Sprintf("wss://%s.cn-beijing.maas.aliyuncs.com/api-ws/v1/realtime?model=%s",
		c.WorkspaceID, url.QueryEscape(c.ASRModel))
}

func (s *asrRealtimeStream) Append(ctx context.Context, pcm []byte) error {
	if len(pcm) == 0 || len(pcm)%2 != 0 {
		return errors.New("realtime ASR PCM must contain whole 16-bit samples")
	}
	if s.finished {
		return errors.New("realtime ASR input is already committed")
	}
	return s.writeJSON(ctx, map[string]any{
		"event_id": eventID(), "type": "input_audio_buffer.append",
		"audio": base64.StdEncoding.EncodeToString(pcm),
	})
}

func (s *asrRealtimeStream) Commit(ctx context.Context) (string, error) {
	if ctx == nil {
		return "", errors.New("context is required")
	}
	if s.finished {
		return "", errors.New("realtime ASR input is already committed")
	}
	s.finished = true
	for _, kind := range []string{"input_audio_buffer.commit", "session.finish"} {
		if err := s.writeJSON(ctx, map[string]any{"event_id": eventID(), "type": kind}); err != nil {
			return "", err
		}
	}
	watchDone := make(chan struct{})
	go func() {
		select {
		case <-ctx.Done():
			_ = s.connection.Close()
		case <-watchDone:
		}
	}()
	defer close(watchDone)

	var transcript string
	for {
		event, err := s.readEvent(ctx)
		if err != nil {

			return "", err
		}
		switch event.Type {
		case "conversation.item.input_audio_transcription.completed":
			transcript = strings.TrimSpace(event.Transcript)
		case "conversation.item.input_audio_transcription.failed", "error":
			return "", fmt.Errorf("Qwen realtime ASR provider_code=%q provider_message=%q",
				event.Error.Code, event.Error.Message)
		case "session.finished":
			if transcript == "" {
				return "", errors.New("Qwen realtime ASR returned no transcript")
			}
			return transcript, nil
		}
	}
}

func (s *asrRealtimeStream) waitFor(ctx context.Context, wanted string) error {
	for {
		event, err := s.readEvent(ctx)
		if err != nil {
			return err
		}
		if event.Type == wanted {
			return nil
		}
		if event.Type == "error" {
			return fmt.Errorf("Qwen realtime ASR provider_code=%q provider_message=%q",
				event.Error.Code, event.Error.Message)
		}
	}
}

func (s *asrRealtimeStream) readEvent(ctx context.Context) (realtimeASREvent, error) {
	if err := ctx.Err(); err != nil {
		return realtimeASREvent{}, err
	}
	if err := s.connection.SetReadDeadline(time.Now().Add(s.config.Timeout)); err != nil {
		return realtimeASREvent{}, err
	}
	messageType, encoded, err := s.connection.ReadMessage()
	if err != nil {
		if ctx.Err() != nil {
			return realtimeASREvent{}, ctx.Err()
		}
		return realtimeASREvent{}, err
	}
	if messageType != websocket.TextMessage || len(encoded) > realtimeASRMaxMessageBytes {
		return realtimeASREvent{}, errors.New("Qwen realtime ASR returned an invalid event")
	}
	var event realtimeASREvent
	if err := json.Unmarshal(encoded, &event); err != nil {
		return realtimeASREvent{}, fmt.Errorf("decode Qwen realtime ASR event: %w", err)
	}
	if event.Type == "" {
		return realtimeASREvent{}, errors.New("Qwen realtime ASR event has no type")
	}
	return event, nil
}

func (s *asrRealtimeStream) writeJSON(ctx context.Context, value any) error {
	if ctx == nil {
		return errors.New("context is required")
	}
	if err := ctx.Err(); err != nil {
		return err
	}
	deadline := time.Now().Add(s.config.Timeout)
	if until, ok := ctx.Deadline(); ok && until.Before(deadline) {
		deadline = until
	}
	if err := s.connection.SetWriteDeadline(deadline); err != nil {
		return err
	}
	stop := context.AfterFunc(ctx, s.Close)
	defer stop()
	err := s.connection.WriteJSON(value)
	if ctx.Err() != nil {
		return ctx.Err()
	}
	return err
}

func (s *asrRealtimeStream) Close() {
	s.closeOnce.Do(func() { _ = s.connection.Close() })
}
