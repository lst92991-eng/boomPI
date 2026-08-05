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
	realtimeASRQueueFrames     = 32 // A bounded 640 ms bridge at 20 ms/frame.
	realtimeASRMaxMessageBytes = 256 * 1024
)

const realtimeASRConnectTimeout = 10 * time.Second

var errRealtimeASRBackpressure = errors.New("Qwen realtime ASR queue is full")

type realtimeASRCommand struct {
	audio  []byte
	finish bool
}

// asrRealtimeStream owns one provider session and therefore one utterance.
// Device audio enters a bounded queue so a slow provider write never blocks
// the device WebSocket reader or grows memory without limit.
type asrRealtimeStream struct {
	config     Config
	connection *websocket.Conn
	commands   chan realtimeASRCommand
	stopped    chan struct{}
	writerDone chan error

	mu           sync.Mutex
	finishQueued bool
	closeOnce    sync.Once
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
		commands:   make(chan realtimeASRCommand, realtimeASRQueueFrames),
		stopped:    make(chan struct{}),
		writerDone: make(chan error, 1),
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
	if err := stream.writeJSON(update); err != nil {
		stream.Close()
		return nil, err
	}
	if err := stream.waitFor(connectCtx, "session.updated"); err != nil {
		stream.Close()
		return nil, err
	}
	stream.config = config
	go stream.writeLoop()
	return stream, nil
}

func (c Config) asrRealtimeURL() string {
	if strings.TrimSpace(c.WorkspaceID) == "" {
		return fmt.Sprintf("wss://dashscope.aliyuncs.com/api-ws/v1/realtime?model=%s",
			url.QueryEscape(realtimeASRModelName))
	}
	return fmt.Sprintf("wss://%s.cn-beijing.maas.aliyuncs.com/api-ws/v1/realtime?model=%s",
		c.WorkspaceID, url.QueryEscape(realtimeASRModelName))
}

func (s *asrRealtimeStream) Append(pcm []byte) error {
	if len(pcm) == 0 || len(pcm)%2 != 0 {
		return errors.New("realtime ASR PCM must contain whole 16-bit samples")
	}
	s.mu.Lock()
	finished := s.finishQueued
	s.mu.Unlock()
	if finished {
		return errors.New("realtime ASR input is already committed")
	}
	copyPCM := append([]byte(nil), pcm...)
	select {
	case s.commands <- realtimeASRCommand{audio: copyPCM}:
		return nil
	case err := <-s.writerDone:
		if err == nil {
			err = errors.New("realtime ASR writer stopped")
		}
		return err
	case <-s.stopped:
		return errors.New("realtime ASR stream is closed")
	default:
		return errRealtimeASRBackpressure
	}
}

func (s *asrRealtimeStream) Commit(ctx context.Context) (string, error) {
	if ctx == nil {
		return "", errors.New("context is required")
	}
	s.mu.Lock()
	if s.finishQueued {
		s.mu.Unlock()
		return "", errors.New("realtime ASR input is already committed")
	}
	s.finishQueued = true
	s.mu.Unlock()

	select {
	case s.commands <- realtimeASRCommand{finish: true}:
	case err := <-s.writerDone:
		if err == nil {
			err = errors.New("realtime ASR writer ended before commit")
		}
		return "", err
	case <-ctx.Done():
		return "", ctx.Err()
	case <-s.stopped:
		return "", errors.New("realtime ASR stream is closed")
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
			select {
			case writerErr := <-s.writerDone:
				if writerErr != nil {
					return "", writerErr
				}
			default:
			}
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

func (s *asrRealtimeStream) writeLoop() {
	for {
		select {
		case <-s.stopped:
			s.writerDone <- context.Canceled
			return
		case command := <-s.commands:
			if command.finish {
				err := s.writeJSON(map[string]any{
					"event_id": eventID(), "type": "input_audio_buffer.commit",
				})
				if err == nil {
					err = s.writeJSON(map[string]any{
						"event_id": eventID(), "type": "session.finish",
					})
				}
				s.writerDone <- err
				if err != nil {
					_ = s.connection.Close()
				}
				return
			}
			err := s.writeJSON(map[string]any{
				"event_id": eventID(),
				"type":     "input_audio_buffer.append",
				"audio":    base64.StdEncoding.EncodeToString(command.audio),
			})
			for i := range command.audio {
				command.audio[i] = 0
			}
			if err != nil {
				s.writerDone <- err
				_ = s.connection.Close()
				return
			}
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

func (s *asrRealtimeStream) writeJSON(value any) error {
	if err := s.connection.SetWriteDeadline(time.Now().Add(s.config.Timeout)); err != nil {
		return err
	}
	return s.connection.WriteJSON(value)
}

func (s *asrRealtimeStream) Close() {
	s.closeOnce.Do(func() {
		close(s.stopped)
		_ = s.connection.Close()
	})
}
