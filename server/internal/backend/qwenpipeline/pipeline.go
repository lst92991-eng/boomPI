package qwenpipeline

import (
	"context"
	"errors"
	"fmt"
	"log/slog"
	"strings"
	"sync"
	"time"

	"github.com/lst92991-eng/boomPI/server/internal/backend"
)

const maxInputPCMBytes = 60 * 16000 * 2

type Backend struct {
	config Config
}

func New(config Config) (*Backend, error) {
	config.APIKey = strings.TrimSpace(config.APIKey)
	config.WorkspaceID = strings.TrimSpace(config.WorkspaceID)
	config.Region = strings.ToLower(strings.TrimSpace(config.Region))
	if err := config.validate(); err != nil {
		return nil, fmt.Errorf("configure Qwen intelligence pipeline: %w", err)
	}
	return &Backend{config: config}, nil
}

func (b *Backend) Open(ctx context.Context, cfg backend.SessionConfig) (backend.ConversationSession, error) {
	if ctx == nil {
		return nil, errors.New("context is required")
	}
	sessionCtx, cancel := context.WithCancel(ctx)
	return &Session{
		config:        b.config,
		sessionConfig: cfg,
		http:          newHTTPClients(b.config),
		tts:           newTTSClient(b.config),
		ctx:           sessionCtx,
		cancel:        cancel,
		events:        make(chan backend.ConversationEvent, b.config.QueueSize),
	}, nil
}

type Session struct {
	config        Config
	sessionConfig backend.SessionConfig
	http          *httpClients
	tts           *ttsClient
	ctx           context.Context
	cancel        context.CancelFunc
	events        chan backend.ConversationEvent

	mu           sync.Mutex
	pcm          []byte
	history      []chatMessage
	activeCancel context.CancelFunc
	activeDone   chan struct{}
	closed       bool
	closeOnce    sync.Once
}

var _ backend.ConversationSession = (*Session)(nil)
var _ backend.CompletedResponseDiscarder = (*Session)(nil)

func (s *Session) SendAudio(ctx context.Context, pcm []byte) error {
	if ctx == nil || len(pcm) == 0 || len(pcm)%2 != 0 {
		return errors.New("PCM must contain a non-empty whole number of 16-bit samples")
	}
	if err := ctx.Err(); err != nil {
		return err
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.closed {
		return errors.New("Qwen pipeline session is closed")
	}
	if s.activeDone != nil {
		return errors.New("Qwen pipeline response is already active")
	}
	if len(s.pcm)+len(pcm) > maxInputPCMBytes {
		return errors.New("Qwen pipeline input exceeds 60 seconds")
	}
	s.pcm = append(s.pcm, pcm...)
	return nil
}

func (s *Session) Commit(ctx context.Context) error {
	if ctx == nil {
		return errors.New("context is required")
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.closed {
		return errors.New("Qwen pipeline session is closed")
	}
	if s.activeDone != nil {
		return errors.New("Qwen pipeline response is already active")
	}
	if len(s.pcm) == 0 {
		return errors.New("Qwen pipeline input audio is empty")
	}
	pcm := append([]byte(nil), s.pcm...)
	s.pcm = s.pcm[:0]
	jobCtx, cancel := context.WithCancel(s.ctx)
	done := make(chan struct{})
	s.activeCancel = cancel
	s.activeDone = done
	committedAt := time.Now()
	go s.run(jobCtx, pcm, done, committedAt)
	return nil
}

func (s *Session) Cancel(ctx context.Context) error {
	if ctx == nil {
		return errors.New("context is required")
	}
	s.mu.Lock()
	cancel, done := s.activeCancel, s.activeDone
	s.pcm = s.pcm[:0]
	s.mu.Unlock()
	if cancel == nil || done == nil {
		return nil
	}
	cancel()
	select {
	case <-done:
		return nil
	case <-ctx.Done():
		return ctx.Err()
	}
}

func (s *Session) DiscardLastResponse(ctx context.Context) error {
	if ctx == nil {
		return errors.New("context is required")
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	if len(s.history) > 0 && s.history[len(s.history)-1].Role == "assistant" {
		s.history = s.history[:len(s.history)-1]
	}
	return nil
}

func (s *Session) Events() <-chan backend.ConversationEvent { return s.events }

func (s *Session) Close() error {
	s.closeOnce.Do(func() {
		s.mu.Lock()
		s.closed = true
		done := s.activeDone
		s.cancel()
		s.mu.Unlock()
		if done != nil {
			<-done
		}
		close(s.events)
	})
	return nil
}

func (s *Session) run(ctx context.Context, pcm []byte, done chan struct{}, committedAt time.Time) {
	responseID := eventID()
	timing := newTurnTiming(s.config.Logger, responseID, committedAt, len(pcm))
	status := "canceled"
	failureStage := ""
	defer func() {
		timing.log(status, failureStage, time.Now())
	}()
	defer func() {
		s.mu.Lock()
		if s.activeDone == done {
			s.activeCancel = nil
			s.activeDone = nil
		}
		s.mu.Unlock()
		close(done)
	}()

	if !s.emit(ctx, backend.ConversationEvent{Type: backend.EventStarted, ResponseID: responseID}) {
		status, failureStage = turnFailure(ctx, "event_delivery")
		return
	}
	transcript, err := s.http.transcribe(ctx, pcm)
	if err != nil {
		status, failureStage = turnFailure(ctx, "asr")
		s.emitError(ctx, err)
		return
	}
	timing.markASRDone(time.Now())

	s.mu.Lock()
	history := append([]chatMessage(nil), s.history...)
	s.mu.Unlock()
	history = append(history, chatMessage{Role: "user", Content: transcript})
	instructions := strings.TrimSpace(s.sessionConfig.SystemPrompt)
	if persona := strings.TrimSpace(s.sessionConfig.Persona); persona != "" {
		instructions += "\n\n" + persona
	}
	ttsCtx, stopTTS := context.WithCancel(ctx)
	defer stopTTS()
	ttsFragments := make(chan string, 64)
	ttsResult := make(chan error, 1)
	go func() {
		ttsResult <- s.tts.synthesizeStream(ttsCtx, ttsFragments, func(pcm []byte) error {
			timing.markTTSFirstPCM(time.Now())
			if !s.emit(ctx, backend.ConversationEvent{
				Type: backend.EventAudio, ResponseID: responseID,
				PCM: append([]byte(nil), pcm...), SampleRateHz: 24000,
			}) {
				return ctx.Err()
			}
			return nil
		})
	}()

	ttsFinished := false
	var ttsErr error
	sendTTS := func(fragment string) error {
		if fragment == "" {
			return nil
		}
		select {
		case ttsFragments <- fragment:
			return nil
		case ttsErr = <-ttsResult:
			ttsFinished = true
			if ttsErr == nil {
				return errors.New("Qwen TTS ended before reasoning output completed")
			}
			return ttsErr
		case <-ctx.Done():
			return ctx.Err()
		}
	}

	answer, reasoningErr := s.http.completeStream(ctx, instructions, history, func(delta string) error {
		timing.markLLMFirstDelta(time.Now())
		if !s.emit(ctx, backend.ConversationEvent{
			Type: backend.EventTextDelta, ResponseID: responseID, Text: delta,
		}) {
			return ctx.Err()
		}
		for _, fragment := range streamingTTSFragments(delta) {
			if err := sendTTS(fragment); err != nil {
				return err
			}
		}
		return nil
	})
	close(ttsFragments)
	if reasoningErr != nil {
		stopTTS()
	}
	if !ttsFinished {
		ttsErr = <-ttsResult
	}
	if reasoningErr != nil {
		status, failureStage = turnFailure(ctx, "llm_stream_or_tts_input")
		if ctx.Err() == nil {
			s.emitError(ctx, reasoningErr)
		}
		return
	}
	if ttsErr != nil {
		status, failureStage = turnFailure(ctx, "tts")
		if ctx.Err() == nil {
			s.emitError(ctx, ttsErr)
		}
		return
	}

	s.mu.Lock()
	s.history = append(history, chatMessage{Role: "assistant", Content: answer})
	if len(s.history) > 40 {
		s.history = append([]chatMessage(nil), s.history[len(s.history)-40:]...)
	}
	s.mu.Unlock()
	if !s.emit(ctx, backend.ConversationEvent{Type: backend.EventDone, ResponseID: responseID}) {
		status, failureStage = turnFailure(ctx, "event_delivery")
		return
	}
	status = "completed"
}

func streamingTTSFragments(delta string) []string {
	if delta == "" {
		return nil
	}
	var fragments []string
	start := 0
	units := 0
	for index, r := range delta {
		nextUnits := units + ttsRuneUnits(r)
		if nextUnits > maxTTSFragmentUnits {
			fragments = append(fragments, delta[start:index])
			start = index
			units = 0
		}
		units += ttsRuneUnits(r)
	}
	fragments = append(fragments, delta[start:])
	return fragments
}

type turnTiming struct {
	logger       *slog.Logger
	responseID   string
	committedAt  time.Time
	inputAudioMS int64

	mu              sync.Mutex
	asrDoneAt       time.Time
	llmFirstDeltaAt time.Time
	ttsFirstPCMAt   time.Time
}

func newTurnTiming(logger *slog.Logger, responseID string, committedAt time.Time, inputPCMBytes int) *turnTiming {
	return &turnTiming{
		logger:       logger,
		responseID:   responseID,
		committedAt:  committedAt,
		inputAudioMS: int64(inputPCMBytes) * 1000 / (16000 * 2),
	}
}

func (t *turnTiming) markASRDone(at time.Time) {
	t.mu.Lock()
	if t.asrDoneAt.IsZero() {
		t.asrDoneAt = at
	}
	t.mu.Unlock()
}

func (t *turnTiming) markLLMFirstDelta(at time.Time) {
	t.mu.Lock()
	if t.llmFirstDeltaAt.IsZero() {
		t.llmFirstDeltaAt = at
	}
	t.mu.Unlock()
}

func (t *turnTiming) markTTSFirstPCM(at time.Time) {
	t.mu.Lock()
	if t.ttsFirstPCMAt.IsZero() {
		t.ttsFirstPCMAt = at
	}
	t.mu.Unlock()
}

func (t *turnTiming) log(status, failureStage string, doneAt time.Time) {
	if t.logger == nil {
		return
	}
	t.mu.Lock()
	asrDoneAt := t.asrDoneAt
	llmFirstDeltaAt := t.llmFirstDeltaAt
	ttsFirstPCMAt := t.ttsFirstPCMAt
	t.mu.Unlock()

	attributes := []any{
		"component", "qwen_pipeline",
		"response_id", t.responseID,
		"status", status,
		"input_audio_ms", t.inputAudioMS,
		"commit_to_done_ms", elapsedMilliseconds(t.committedAt, doneAt),
	}
	if failureStage != "" {
		attributes = append(attributes, "failure_stage", failureStage)
	}
	if !asrDoneAt.IsZero() {
		attributes = append(attributes,
			"commit_to_asr_done_ms", elapsedMilliseconds(t.committedAt, asrDoneAt))
	}
	if !llmFirstDeltaAt.IsZero() {
		attributes = append(attributes,
			"commit_to_llm_first_delta_ms", elapsedMilliseconds(t.committedAt, llmFirstDeltaAt))
		if !asrDoneAt.IsZero() {
			attributes = append(attributes,
				"asr_done_to_llm_first_delta_ms", elapsedMilliseconds(asrDoneAt, llmFirstDeltaAt))
		}
	}
	if !ttsFirstPCMAt.IsZero() {
		attributes = append(attributes,
			"commit_to_tts_first_pcm_ms", elapsedMilliseconds(t.committedAt, ttsFirstPCMAt))
		if !llmFirstDeltaAt.IsZero() {
			attributes = append(attributes,
				"llm_first_delta_to_tts_first_pcm_ms", elapsedMilliseconds(llmFirstDeltaAt, ttsFirstPCMAt))
		}
	}
	t.logger.Info("Qwen pipeline turn timing", attributes...)
}

func elapsedMilliseconds(start, end time.Time) int64 {
	if start.IsZero() || end.Before(start) {
		return 0
	}
	return end.Sub(start).Milliseconds()
}

func turnFailure(ctx context.Context, stage string) (string, string) {
	if ctx.Err() != nil {
		return "canceled", stage
	}
	return "failed", stage
}

func (s *Session) emit(ctx context.Context, event backend.ConversationEvent) bool {
	select {
	case s.events <- event:
		return true
	case <-ctx.Done():
		return false
	case <-s.ctx.Done():
		return false
	}
}

func (s *Session) emitError(ctx context.Context, err error) {
	if err == nil || ctx.Err() != nil {
		return
	}
	_ = s.emit(ctx, backend.ConversationEvent{Type: backend.EventError, Err: err})
}
