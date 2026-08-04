package qwenpipeline

import (
	"context"
	"errors"
	"fmt"
	"log/slog"
	"net"
	"strings"
	"sync"
	"time"
	"unicode"

	"github.com/lst92991-eng/boomPI/server/internal/backend"
)

const maxInputPCMBytes = 60 * 16000 * 2

const clearConversationConfirmation = "对话已清空。"

type ttsStreamSynthesizer interface {
	synthesizeStream(context.Context, <-chan string, func([]byte) error) error
}

type Backend struct {
	config Config
}

func New(config Config) (*Backend, error) {
	config.APIKey = strings.TrimSpace(config.APIKey)
	config.WorkspaceID = strings.TrimSpace(config.WorkspaceID)
	config.Region = strings.ToLower(strings.TrimSpace(config.Region))
	if config.MaxTurns == 0 {
		config.MaxTurns = 20
	}
	if config.MaxContextTokens == 0 {
		config.MaxContextTokens = 24_000
	}
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
	session := &Session{
		config:        b.config,
		sessionConfig: cfg,
		http:          newHTTPClients(b.config),
		tts:           newTTSClient(b.config),
		ctx:           sessionCtx,
		cancel:        cancel,
		events:        make(chan backend.ConversationEvent, b.config.QueueSize),
	}
	stream, err := openRealtimeASR(sessionCtx, b.config)
	if err != nil {
		if b.config.Logger != nil {
			b.config.Logger.Warn("Qwen realtime ASR unavailable; using bounded batch fallback",
				"component", "qwen_pipeline", "error_code", qwenPipelineErrorCode(err))
		}
	} else {
		session.asr = stream
	}
	return session, nil
}

type Session struct {
	config        Config
	sessionConfig backend.SessionConfig
	http          *httpClients
	tts           ttsStreamSynthesizer
	ctx           context.Context
	cancel        context.CancelFunc
	events        chan backend.ConversationEvent

	mu            sync.Mutex
	pcm           []byte
	history       []chatMessage
	activeCancel  context.CancelFunc
	activeDone    chan struct{}
	asr           *asrRealtimeStream
	asrPreparing  bool
	turnBatchOnly bool
	// True only after the newest response has been committed to history and
	// before the next user turn starts. This closes the race where provider
	// generation finishes before the device has finished playing the answer:
	// a barge-in must still remove that unheard response from context.
	lastResponseDiscardable bool
	closed                  bool
	closeOnce               sync.Once
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
	if s.closed {
		s.mu.Unlock()
		return errors.New("Qwen pipeline session is closed")
	}
	if s.activeDone != nil {
		s.mu.Unlock()
		return errors.New("Qwen pipeline response is already active")
	}
	if len(s.pcm)+len(pcm) > maxInputPCMBytes {
		s.mu.Unlock()
		return errors.New("Qwen pipeline input exceeds 60 seconds")
	}
	if len(s.pcm) == 0 && s.asr == nil {
		s.turnBatchOnly = true
	}
	if len(s.pcm) == 0 {
		s.lastResponseDiscardable = false
	}
	s.pcm = append(s.pcm, pcm...)
	stream := s.asr
	if s.turnBatchOnly {
		stream = nil
	}
	s.mu.Unlock()
	if stream != nil {
		if err := stream.Append(pcm); err != nil {
			s.disableRealtimeASR(stream, err)
		}
	}
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
	clear(s.pcm)
	s.pcm = s.pcm[:0]
	stream := s.asr
	if s.turnBatchOnly {
		stream = nil
	} else {
		s.asr = nil
	}
	s.turnBatchOnly = false
	// A live stream is replaced after its final result is received. Starting
	// its replacement here would briefly overlap two provider ASR sessions and
	// can hit provider connection limits. Batch-only turns have no such overlap.
	if stream == nil && s.asr == nil && !s.asrPreparing {
		s.asrPreparing = true
		go s.prepareRealtimeASR()
	}
	jobCtx, cancel := context.WithCancel(s.ctx)
	done := make(chan struct{})
	s.activeCancel = cancel
	s.activeDone = done
	committedAt := time.Now()
	go s.run(jobCtx, pcm, stream, done, committedAt)
	return nil
}

func (s *Session) Cancel(ctx context.Context) error {
	if ctx == nil {
		return errors.New("context is required")
	}
	s.mu.Lock()
	cancel, done := s.activeCancel, s.activeDone
	inputWasActive := len(s.pcm) != 0
	discardCompletedResponse := !inputWasActive
	var stream *asrRealtimeStream
	if inputWasActive && !s.turnBatchOnly {
		stream = s.asr
		s.asr = nil
	}
	clear(s.pcm)
	s.pcm = s.pcm[:0]
	s.turnBatchOnly = false
	if stream != nil && !s.asrPreparing {
		s.asrPreparing = true
		go s.prepareRealtimeASR()
	}
	s.mu.Unlock()
	if stream != nil {
		stream.Close()
	}
	if cancel == nil || done == nil {
		if discardCompletedResponse {
			s.discardCompletedResponseIfNeeded()
		}
		return nil
	}
	cancel()
	select {
	case <-done:
		if discardCompletedResponse {
			s.discardCompletedResponseIfNeeded()
		}
		return nil
	case <-ctx.Done():
		return ctx.Err()
	}
}

func (s *Session) DiscardLastResponse(ctx context.Context) error {
	if ctx == nil {
		return errors.New("context is required")
	}
	s.discardCompletedResponseIfNeeded()
	return nil
}

func (s *Session) discardCompletedResponseIfNeeded() {
	s.mu.Lock()
	defer s.mu.Unlock()
	if !s.lastResponseDiscardable {
		return
	}
	if len(s.history) > 0 && s.history[len(s.history)-1].Role == "assistant" {
		s.history = s.history[:len(s.history)-1]
		if len(s.history) > 0 && s.history[len(s.history)-1].Role == "user" {
			s.history = s.history[:len(s.history)-1]
		}
	}
	s.lastResponseDiscardable = false
}

func (s *Session) Events() <-chan backend.ConversationEvent { return s.events }

func (s *Session) Close() error {
	s.closeOnce.Do(func() {
		s.mu.Lock()
		s.closed = true
		done := s.activeDone
		stream := s.asr
		s.asr = nil
		s.cancel()
		s.mu.Unlock()
		if stream != nil {
			stream.Close()
		}
		if done != nil {
			<-done
		}
		close(s.events)
	})
	return nil
}

func (s *Session) run(ctx context.Context, pcm []byte, asr *asrRealtimeStream, done chan struct{}, committedAt time.Time) {
	responseID := eventID()
	timing := newTurnTiming(s.config.Logger, responseID, committedAt, len(pcm))
	status := "canceled"
	failureStage := ""
	defer func() {
		timing.log(status, failureStage, time.Now())
	}()
	defer func() {
		clear(pcm)
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
	var transcript string
	var err error
	if asr != nil {
		transcript, err = asr.Commit(ctx)
		asr.Close()
		s.startRealtimeASRPreparation()
		if err != nil && ctx.Err() == nil && s.config.Logger != nil {
			s.config.Logger.Warn("Qwen realtime ASR failed; retrying this turn with batch ASR",
				"component", "qwen_pipeline", "response_id", responseID,
				"error_code", qwenPipelineErrorCode(err))
		}
	}
	if asr == nil || err != nil {
		if ctx.Err() == nil {
			transcript, err = s.http.transcribe(ctx, pcm)
		}
	}
	if err != nil {
		status, failureStage = turnFailure(ctx, "asr")
		s.emitError(ctx, responseID, "asr", err)
		return
	}
	timing.markASRDone(time.Now())
	if handled, commandErr := s.handleClearConversation(ctx, responseID, transcript, timing); handled {
		if commandErr != nil {
			status, failureStage = turnFailure(ctx, "clear_conversation")
			if ctx.Err() == nil {
				s.emitError(ctx, responseID, "clear_conversation", commandErr)
			}
			return
		}
		status = "completed"
		return
	}

	s.mu.Lock()
	history := append([]chatMessage(nil), s.history...)
	s.mu.Unlock()
	history = append(history, chatMessage{Role: "user", Content: transcript})
	instructions := strings.TrimSpace(s.sessionConfig.SystemPrompt)
	if persona := strings.TrimSpace(s.sessionConfig.Persona); persona != "" {
		instructions += "\n\n" + persona
	}
	history = boundedHistory(history, s.config.MaxTurns,
		s.config.MaxContextTokens, instructions)
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
	timing.markLLMDone(time.Now())
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
			s.emitError(ctx, responseID, "llm_stream_or_tts_input", reasoningErr)
		}
		return
	}
	if ttsErr != nil {
		status, failureStage = turnFailure(ctx, "tts")
		if ctx.Err() == nil {
			s.emitError(ctx, responseID, "tts", ttsErr)
		}
		return
	}

	s.mu.Lock()
	s.history = boundedHistory(
		append(history, chatMessage{Role: "assistant", Content: answer}),
		s.config.MaxTurns, s.config.MaxContextTokens, instructions)
	s.lastResponseDiscardable = true
	s.mu.Unlock()
	if !s.emit(ctx, backend.ConversationEvent{Type: backend.EventDone, ResponseID: responseID}) {
		status, failureStage = turnFailure(ctx, "event_delivery")
		return
	}
	status = "completed"
}

func (s *Session) handleClearConversation(
	ctx context.Context,
	responseID string,
	transcript string,
	timing *turnTiming,
) (bool, error) {
	if !isClearConversationCommand(transcript) {
		return false, nil
	}

	// Clear the old context before any confirmation I/O. Even if TTS fails or
	// the device disconnects, the next turn must not recover the old history.
	s.mu.Lock()
	clear(s.history)
	s.history = nil
	s.lastResponseDiscardable = false
	s.mu.Unlock()

	if !s.emit(ctx, backend.ConversationEvent{
		Type: backend.EventTextDelta, ResponseID: responseID, Text: clearConversationConfirmation,
	}) {
		return true, context.Canceled
	}
	fragments := make(chan string, 1)
	fragments <- clearConversationConfirmation
	close(fragments)
	if err := s.tts.synthesizeStream(ctx, fragments, func(pcm []byte) error {
		timing.markTTSFirstPCM(time.Now())
		if !s.emit(ctx, backend.ConversationEvent{
			Type: backend.EventAudio, ResponseID: responseID,
			PCM: append([]byte(nil), pcm...), SampleRateHz: 24000,
		}) {
			return context.Canceled
		}
		return nil
	}); err != nil {
		return true, fmt.Errorf("synthesize clear-conversation confirmation: %w", err)
	}
	if !s.emit(ctx, backend.ConversationEvent{Type: backend.EventDone, ResponseID: responseID}) {
		return true, context.Canceled
	}
	return true, nil
}

func isClearConversationCommand(transcript string) bool {
	normalized := strings.Map(func(current rune) rune {
		if unicode.IsSpace(current) || unicode.IsPunct(current) {
			return -1
		}
		return current
	}, transcript)
	return normalized == "清空对话"
}

func (s *Session) disableRealtimeASR(stream *asrRealtimeStream, cause error) {
	s.mu.Lock()
	if s.asr == stream {
		s.asr = nil
		s.turnBatchOnly = true
	}
	s.mu.Unlock()
	stream.Close()
	if s.config.Logger != nil {
		s.config.Logger.Warn("Qwen realtime ASR input fell back to batch mode",
			"component", "qwen_pipeline", "error_code", qwenPipelineErrorCode(cause))
	}
}

func (s *Session) prepareRealtimeASR() {
	stream, err := openRealtimeASR(s.ctx, s.config)
	s.mu.Lock()
	s.asrPreparing = false
	if err == nil && !s.closed && s.asr == nil {
		s.asr = stream
		stream = nil
	}
	s.mu.Unlock()
	if stream != nil {
		stream.Close()
	}
	if err != nil && s.config.Logger != nil && s.ctx.Err() == nil {
		s.config.Logger.Warn("Qwen realtime ASR preconnect failed; next turn will use batch fallback",
			"component", "qwen_pipeline", "error_code", qwenPipelineErrorCode(err))
	}
}

func (s *Session) startRealtimeASRPreparation() {
	s.mu.Lock()
	if s.closed || s.asr != nil || s.asrPreparing {
		s.mu.Unlock()
		return
	}
	s.asrPreparing = true
	s.mu.Unlock()
	go s.prepareRealtimeASR()
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

// boundedHistory makes MaxTurns a rolling context window, not a session life
// limit. The token estimate is deliberately conservative for mixed Chinese
// and English so a device can converse indefinitely without eventually
// overflowing the provider context.
func boundedHistory(history []chatMessage, maxTurns, maxContextTokens int, instructions string) []chatMessage {
	if len(history) == 0 {
		return nil
	}
	maxMessages := maxTurns * 2
	// During generation, the newest user message has no assistant partner yet.
	// Reserve one slot for that unmatched message instead of slicing off the
	// user half of the oldest complete pair and leaving an orphan assistant.
	if history[len(history)-1].Role == "user" {
		maxMessages--
	}
	if maxMessages < 2 {
		maxMessages = 1
	}
	if len(history) > maxMessages {
		history = history[len(history)-maxMessages:]
	}
	for len(history) > 1 && history[0].Role == "assistant" {
		history = history[1:]
	}
	reserve := maxContextTokens / 4
	if reserve < 512 {
		reserve = 512
	}
	budget := maxContextTokens - reserve - estimateTextTokens(instructions)
	if budget < 256 {
		budget = 256
	}
	for len(history) > 1 && estimateHistoryTokens(history) > budget {
		remove := 1
		if len(history) >= 2 && history[0].Role == "user" && history[1].Role == "assistant" {
			remove = 2
		}
		history = history[remove:]
	}
	return append([]chatMessage(nil), history...)
}

func estimateHistoryTokens(history []chatMessage) int {
	total := 0
	for _, message := range history {
		total += 8 + estimateTextTokens(message.Content)
	}
	return total
}

func estimateTextTokens(text string) int {
	ascii := 0
	tokens := 0
	for _, value := range text {
		if value <= 0x7f {
			ascii++
		} else {
			// Two tokens per non-ASCII rune errs on the safe side for CJK,
			// emoji, and mixed-language voice transcripts.
			tokens += 2
		}
	}
	return tokens + (ascii+3)/4
}

type turnTiming struct {
	logger       *slog.Logger
	responseID   string
	committedAt  time.Time
	inputAudioMS int64

	mu              sync.Mutex
	asrDoneAt       time.Time
	llmFirstDeltaAt time.Time
	llmDoneAt       time.Time
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

func (t *turnTiming) markLLMDone(at time.Time) {
	t.mu.Lock()
	if t.llmDoneAt.IsZero() {
		t.llmDoneAt = at
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
	llmDoneAt := t.llmDoneAt
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
	if !llmDoneAt.IsZero() {
		attributes = append(attributes,
			"commit_to_llm_done_ms", elapsedMilliseconds(t.committedAt, llmDoneAt))
		if !llmFirstDeltaAt.IsZero() {
			attributes = append(attributes,
				"llm_first_delta_to_done_ms", elapsedMilliseconds(llmFirstDeltaAt, llmDoneAt))
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

func (s *Session) emitError(ctx context.Context, responseID, stage string, err error) {
	if err == nil || ctx.Err() != nil {
		return
	}
	if s.config.Logger != nil {
		s.config.Logger.Error("Qwen pipeline turn failed", "component", "qwen_pipeline",
			"response_id", responseID, "stage", stage,
			"error_code", qwenPipelineErrorCode(err))
	}
	_ = s.emit(ctx, backend.ConversationEvent{Type: backend.EventError, Err: err})
}

func qwenPipelineErrorCode(err error) string {
	if errors.Is(err, context.Canceled) {
		return "request_canceled"
	}
	if errors.Is(err, context.DeadlineExceeded) {
		return "request_timeout"
	}
	var networkError net.Error
	if errors.As(err, &networkError) {
		if networkError.Timeout() {
			return "network_timeout"
		}
		return "network_error"
	}
	return "provider_request_failed"
}
