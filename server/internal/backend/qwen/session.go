package qwen

import (
	"context"
	"encoding/base64"
	"encoding/json"
	"errors"
	"fmt"
	"sync"
	"sync/atomic"
	"time"

	"github.com/gorilla/websocket"
	"github.com/lst92991-eng/boomPI/server/internal/backend"
)

var (
	ErrSessionClosed = errors.New("qwen session is closed")
	ErrBackpressure  = errors.New("qwen outbound queue is full")
)

var errResponsePublishCanceled = errors.New("qwen response publication canceled")

type clientEvent struct {
	Type    string         `json:"type"`
	Audio   string         `json:"audio,omitempty"`
	ItemID  string         `json:"item_id,omitempty"`
	Session *sessionUpdate `json:"session,omitempty"`
}

type sessionUpdate struct {
	Modalities        []string `json:"modalities"`
	Voice             string   `json:"voice"`
	Instructions      string   `json:"instructions,omitempty"`
	InputAudioFormat  string   `json:"input_audio_format"`
	OutputAudioFormat string   `json:"output_audio_format"`
	TurnDetection     any      `json:"turn_detection"`
}

type serverEvent struct {
	Type       string `json:"type"`
	Delta      string `json:"delta"`
	ResponseID string `json:"response_id"`
	Response   struct {
		ID     string `json:"id"`
		Status string `json:"status"`
		Output []struct {
			ID   string `json:"id"`
			Role string `json:"role"`
		} `json:"output"`
	} `json:"response"`
	Item struct {
		ID   string `json:"id"`
		Role string `json:"role"`
	} `json:"item"`
	Error struct {
		Type    string `json:"type"`
		Code    string `json:"code"`
		Message string `json:"message"`
	} `json:"error"`
}

type outboundBatch struct {
	events          []clientEvent
	inputAudio      bool
	inputGeneration uint64
}

type Session struct {
	config Config
	conn   *websocket.Conn

	ctx          context.Context
	cancel       context.CancelFunc
	writes       chan outboundBatch
	urgentWrites chan outboundBatch
	events       chan backend.ConversationEvent
	ready        chan error
	done         chan struct{}

	inputGeneration atomic.Uint64

	readyOnce         sync.Once
	readyOK           atomic.Bool
	failOnce          sync.Once
	closeOnce         sync.Once
	wg                sync.WaitGroup
	errMu             sync.RWMutex
	fatalErr          error
	responseMu        sync.Mutex
	responseRequested bool
	responsePublish   chan struct{}
	publishCanceled   bool
	cancelWait        chan error
	lastAssistantItem string
}

var _ backend.ConversationSession = (*Session)(nil)
var _ backend.CompletedResponseDiscarder = (*Session)(nil)

func newSession(config Config, conn *websocket.Conn) *Session {
	ctx, cancel := context.WithCancel(context.Background())
	conn.SetReadLimit(maxMessageBytes)
	return &Session{
		config:       config,
		conn:         conn,
		ctx:          ctx,
		cancel:       cancel,
		writes:       make(chan outboundBatch, config.QueueSize),
		urgentWrites: make(chan outboundBatch, 1),
		events:       make(chan backend.ConversationEvent, config.QueueSize),
		ready:        make(chan error, 1),
		done:         make(chan struct{}),
	}
}

func (s *Session) start() {
	s.wg.Add(2)
	go s.writer()
	go s.reader()
	go func() {
		s.wg.Wait()
		close(s.events)
		close(s.done)
	}()
}

func (s *Session) Events() <-chan backend.ConversationEvent { return s.events }

func (s *Session) SendAudio(ctx context.Context, pcm []byte) error {
	if len(pcm) == 0 || len(pcm)%2 != 0 {
		return errors.New("PCM must contain a non-empty whole number of 16-bit samples")
	}
	if len(pcm) > maxAudioChunkBytes {
		return fmt.Errorf("PCM chunk has %d bytes, limit is %d", len(pcm), maxAudioChunkBytes)
	}
	audio := base64.StdEncoding.EncodeToString(pcm)
	return s.enqueueNow(ctx, outboundBatch{
		events:     []clientEvent{{Type: "input_audio_buffer.append", Audio: audio}},
		inputAudio: true, inputGeneration: s.inputGeneration.Load(),
	})
}

func (s *Session) Commit(ctx context.Context) error {
	s.responseMu.Lock()
	if s.responseRequested {
		s.responseMu.Unlock()
		return errors.New("qwen response is already active")
	}
	s.responseRequested = true
	s.responsePublish = make(chan struct{})
	s.publishCanceled = false
	s.responseMu.Unlock()
	err := s.enqueueNow(ctx, outboundBatch{events: []clientEvent{
		{Type: "input_audio_buffer.commit"},
		{Type: "response.create"},
	}})
	if err != nil {
		s.responseMu.Lock()
		s.responseRequested = false
		s.stopResponsePublishLocked()
		s.responsePublish = nil
		s.publishCanceled = false
		s.responseMu.Unlock()
	}
	return err
}

func (s *Session) Cancel(ctx context.Context) error {
	if ctx == nil {
		ctx = context.Background()
	}
	s.responseMu.Lock()
	if !s.responseRequested {
		s.responseMu.Unlock()
		s.inputGeneration.Add(1)
		return s.enqueueUrgent(ctx, outboundBatch{events: []clientEvent{{Type: "input_audio_buffer.clear"}}})
	}
	if s.cancelWait != nil {
		s.responseMu.Unlock()
		return errors.New("qwen response cancellation is already in progress")
	}
	wait := make(chan error, 1)
	s.cancelWait = wait
	s.stopResponsePublishLocked()
	s.responseMu.Unlock()
	if err := s.enqueue(ctx, outboundBatch{events: []clientEvent{{Type: "response.cancel"}}}); err != nil {
		s.responseMu.Lock()
		if s.cancelWait == wait {
			s.cancelWait = nil
		}
		s.responseMu.Unlock()
		return err
	}
	timer := time.NewTimer(s.config.Timeout)
	defer timer.Stop()
	select {
	case err := <-wait:
		return err
	case <-ctx.Done():
		return transportError(ctx.Err())
	case <-s.ctx.Done():
		return s.closedError()
	case <-timer.C:
		return transportError(context.DeadlineExceeded)
	}
}

func (s *Session) DiscardLastResponse(ctx context.Context) error {
	if ctx == nil {
		ctx = context.Background()
	}
	s.responseMu.Lock()
	itemID := s.lastAssistantItem
	s.lastAssistantItem = ""
	s.responseMu.Unlock()
	if itemID == "" {
		return nil
	}
	return s.enqueueNow(ctx, outboundBatch{events: []clientEvent{{
		Type: "conversation.item.delete", ItemID: itemID,
	}}})
}

func (s *Session) Close() error {
	s.closeOnce.Do(func() {
		s.cancel()
		_ = s.conn.Close()
	})
	timer := time.NewTimer(s.config.Timeout)
	defer timer.Stop()
	select {
	case <-s.done:
		return nil
	case <-timer.C:
		return transportError(context.DeadlineExceeded)
	}
}

func (s *Session) enqueue(ctx context.Context, batch outboundBatch) error {
	if ctx == nil {
		ctx = context.Background()
	}
	select {
	case <-s.ctx.Done():
		return s.closedError()
	default:
	}
	select {
	case s.writes <- batch:
		return nil
	case <-ctx.Done():
		return transportError(ctx.Err())
	case <-s.ctx.Done():
		return s.closedError()
	}
}

func (s *Session) enqueueNow(ctx context.Context, batch outboundBatch) error {
	if ctx == nil {
		ctx = context.Background()
	}
	if err := context.Cause(ctx); err != nil {
		return transportError(err)
	}
	select {
	case <-s.ctx.Done():
		return s.closedError()
	default:
	}
	select {
	case s.writes <- batch:
		return nil
	default:
		return ErrBackpressure
	}
}

func (s *Session) enqueueUrgent(ctx context.Context, batch outboundBatch) error {
	if ctx == nil {
		ctx = context.Background()
	}
	select {
	case s.urgentWrites <- batch:
		return nil
	case <-ctx.Done():
		return transportError(context.Cause(ctx))
	case <-s.ctx.Done():
		return s.closedError()
	}
}

// writer is the only goroutine allowed to write to the WebSocket.
func (s *Session) writer() {
	defer s.wg.Done()
	for {
		var batch outboundBatch
		select {
		case batch = <-s.urgentWrites:
		default:
			select {
			case <-s.ctx.Done():
				return
			case batch = <-s.urgentWrites:
			case batch = <-s.writes:
			}
		}
		if batch.inputAudio && batch.inputGeneration != s.inputGeneration.Load() {
			continue
		}
		for _, event := range batch.events {
			if err := s.conn.SetWriteDeadline(time.Now().Add(s.config.Timeout)); err != nil {
				s.fail(transportError(err))
				return
			}
			if err := s.conn.WriteJSON(event); err != nil {
				if s.ctx.Err() == nil {
					s.fail(transportError(err))
				}
				return
			}
		}
	}
}

// reader is the only goroutine allowed to read from the WebSocket.
func (s *Session) reader() {
	defer s.wg.Done()
	for {
		_, payload, err := s.conn.ReadMessage()
		if err != nil {
			if s.ctx.Err() == nil {
				s.fail(transportError(err))
			}
			return
		}
		var event serverEvent
		if err := json.Unmarshal(payload, &event); err != nil {
			s.fail(transportError(err))
			return
		}
		if err := s.handleServerEvent(event); err != nil {
			s.fail(err)
			return
		}
	}
}

func (s *Session) handleServerEvent(event serverEvent) error {
	switch event.Type {
	case "session.updated":
		s.signalReady(nil)
	case "error":
		err := fmt.Errorf("%w (%s): %s", ErrProvider, event.Error.Code, providerMessage(event))
		if !s.readyOK.Load() {
			s.signalReady(err)
			return err
		}
		if publishErr := s.publishResponseEvent(backend.ConversationEvent{Type: backend.EventError, Err: err}); publishErr != nil {
			return publishErr
		}
		s.finishResponse(err)
		return nil
	case "response.created":
		return s.publishResponseEvent(backend.ConversationEvent{Type: backend.EventStarted, ResponseID: event.Response.ID})
	case "response.output_item.added", "conversation.item.created":
		if event.Item.ID != "" && event.Item.Role == "assistant" {
			s.responseMu.Lock()
			s.lastAssistantItem = event.Item.ID
			s.responseMu.Unlock()
		}
		return nil
	case "response.text.delta", "response.audio_transcript.delta":
		return s.publishResponseEvent(backend.ConversationEvent{Type: backend.EventTextDelta, ResponseID: event.ResponseID, Text: event.Delta})
	case "response.audio.delta":
		pcm, err := base64.StdEncoding.DecodeString(event.Delta)
		if err != nil {
			return transportError(err)
		}
		if len(pcm) == 0 || len(pcm)%2 != 0 {
			return transportError(errors.New("qwen returned unaligned PCM"))
		}
		for offset := 0; offset < len(pcm); offset += outputAudioEventBytes {
			end := offset + outputAudioEventBytes
			if end > len(pcm) {
				end = len(pcm)
			}
			if err := s.publishResponseEvent(backend.ConversationEvent{
				Type: backend.EventAudio, ResponseID: event.ResponseID,
				PCM: append([]byte(nil), pcm[offset:end]...), SampleRateHz: 24_000,
			}); err != nil {
				return err
			}
		}
		return nil
	case "response.done":
		for _, item := range event.Response.Output {
			if item.ID != "" && item.Role == "assistant" {
				s.responseMu.Lock()
				s.lastAssistantItem = item.ID
				s.responseMu.Unlock()
			}
		}
		responseErr := responseStatusError(event.Response.Status)
		eventType := backend.EventDone
		if responseErr != nil {
			eventType = backend.EventError
		}
		if err := s.publishResponseEvent(backend.ConversationEvent{Type: eventType, ResponseID: event.Response.ID, Err: responseErr}); err != nil {
			return err
		}
		s.finishResponse(responseErr)
		return nil
	}
	return nil
}

func responseStatusError(status string) error {
	switch status {
	case "completed", "cancelled":
		return nil
	case "failed", "incomplete":
		return fmt.Errorf("%w: response ended with status %s", ErrProvider, status)
	default:
		return fmt.Errorf("%w: response.done has invalid status %q", ErrProvider, status)
	}
}

func providerMessage(event serverEvent) string {
	if event.Error.Message != "" {
		return event.Error.Message
	}
	if event.Error.Type != "" {
		return event.Error.Type
	}
	return "provider rejected request"
}

func (s *Session) publish(event backend.ConversationEvent) error {
	var sessionDone <-chan struct{}
	if s.ctx != nil {
		sessionDone = s.ctx.Done()
	}
	s.responseMu.Lock()
	responsePublish := s.responsePublish
	s.responseMu.Unlock()
	if responsePublish != nil {
		select {
		case <-responsePublish:
			return errResponsePublishCanceled
		default:
		}
	}
	select {
	case s.events <- event:
		return nil
	case <-responsePublish:
		return errResponsePublishCanceled
	case <-sessionDone:
		return transportError(context.Cause(s.ctx))
	}
}

// A response cancel fences queued deltas before waiting for Qwen's
// acknowledgement. The reader can therefore escape bounded event
// backpressure and continue reading response.done on the same WebSocket.
func (s *Session) publishResponseEvent(event backend.ConversationEvent) error {
	err := s.publish(event)
	if errors.Is(err, errResponsePublishCanceled) {
		return nil
	}
	return err
}

func (s *Session) stopResponsePublishLocked() {
	if s.responsePublish != nil && !s.publishCanceled {
		close(s.responsePublish)
		s.publishCanceled = true
	}
}

func (s *Session) fail(err error) {
	s.failOnce.Do(func() {
		s.errMu.Lock()
		s.fatalErr = err
		s.errMu.Unlock()
		s.signalReady(err)
		s.finishResponse(err)
		s.cancel()
		_ = s.conn.Close()
	})
}

func (s *Session) finishResponse(err error) {
	s.responseMu.Lock()
	s.responseRequested = false
	s.stopResponsePublishLocked()
	s.responsePublish = nil
	s.publishCanceled = false
	if s.cancelWait != nil {
		s.cancelWait <- err
		s.cancelWait = nil
	}
	s.responseMu.Unlock()
}

func (s *Session) signalReady(err error) {
	s.readyOnce.Do(func() {
		if err == nil {
			s.readyOK.Store(true)
		}
		s.ready <- err
	})
}

func (s *Session) closedError() error {
	s.errMu.RLock()
	err := s.fatalErr
	s.errMu.RUnlock()
	if err != nil {
		return err
	}
	return ErrSessionClosed
}
