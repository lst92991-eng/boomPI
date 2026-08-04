package qwenpipeline

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"log/slog"
	"strings"
	"testing"
	"time"

	"github.com/lst92991-eng/boomPI/server/internal/backend"
)

func TestOpenReturnsBeforeRealtimeASRPreparation(t *testing.T) {
	provider, err := New(Config{
		APIKey: "test-key", Region: RegionChinaBeijing,
		ASRModel: "asr", ReasoningModel: "reasoning", ReasoningEffort: "none",
		TTSModel: "tts", TTSVoice: "voice", SearchMode: "off",
		Timeout: time.Second, QueueSize: 8, MaxTurns: 20, MaxContextTokens: 24_000,
	})
	if err != nil {
		t.Fatalf("New() error = %v", err)
	}
	preparationStarted := make(chan struct{})
	preparationStopped := make(chan struct{})
	provider.openASR = func(ctx context.Context, _ Config) (*asrRealtimeStream, error) {
		close(preparationStarted)
		<-ctx.Done()
		close(preparationStopped)
		return nil, ctx.Err()
	}
	type openResult struct {
		session backend.ConversationSession
		err     error
	}
	opened := make(chan openResult, 1)
	go func() {
		session, openErr := provider.Open(context.Background(), backend.SessionConfig{})
		opened <- openResult{session: session, err: openErr}
	}()

	var result openResult
	select {
	case result = <-opened:
		if result.err != nil {
			t.Fatalf("Open() error = %v", result.err)
		}
	case <-time.After(250 * time.Millisecond):
		t.Fatal("Open() waited for realtime ASR and would delay hello.ack")
	}
	select {
	case <-preparationStarted:
	case <-time.After(time.Second):
		t.Fatal("realtime ASR preparation did not start in the background")
	}

	session := result.session.(*Session)
	if err := session.SendAudio(context.Background(), []byte{0, 0}); err != nil {
		t.Fatalf("SendAudio() error = %v", err)
	}
	session.mu.Lock()
	batchOnly := session.turnBatchOnly
	session.mu.Unlock()
	if !batchOnly {
		t.Fatal("first turn did not select batch fallback while realtime ASR was preparing")
	}
	if err := session.Cancel(context.Background()); err != nil {
		t.Fatalf("Cancel() error = %v", err)
	}
	if err := session.Close(); err != nil {
		t.Fatalf("Close() error = %v", err)
	}
	select {
	case <-preparationStopped:
	case <-time.After(time.Second):
		t.Fatal("background realtime ASR preparation survived Session.Close()")
	}
}

func TestTurnTimingWritesOneSanitizedAggregateRecord(t *testing.T) {
	var output bytes.Buffer
	logger := slog.New(slog.NewJSONHandler(&output, nil))
	committedAt := time.Date(2026, time.July, 29, 12, 0, 0, 0, time.UTC)
	timing := newTurnTiming(logger, "response-test", committedAt, 32000)
	timing.markASRMode("realtime")
	timing.markASRDone(committedAt.Add(100 * time.Millisecond))
	timing.markLLMFirstDelta(committedAt.Add(250 * time.Millisecond))
	timing.markLLMDone(committedAt.Add(350 * time.Millisecond))
	timing.markTTSFirstPCM(committedAt.Add(400 * time.Millisecond))
	timing.log("completed", "", committedAt.Add(700*time.Millisecond))

	var record map[string]any
	if err := json.Unmarshal(output.Bytes(), &record); err != nil {
		t.Fatalf("decode timing log: %v; output=%q", err, output.String())
	}
	wantNumbers := map[string]float64{
		"input_audio_ms":                      1000,
		"commit_to_asr_done_ms":               100,
		"commit_to_llm_first_delta_ms":        250,
		"asr_done_to_llm_first_delta_ms":      150,
		"commit_to_tts_first_pcm_ms":          400,
		"llm_first_delta_to_tts_first_pcm_ms": 150,
		"commit_to_llm_done_ms":               350,
		"llm_first_delta_to_done_ms":          100,
		"commit_to_done_ms":                   700,
	}
	for key, want := range wantNumbers {
		if got := record[key]; got != want {
			t.Errorf("%s = %#v, want %.0f", key, got, want)
		}
	}
	if got := record["status"]; got != "completed" {
		t.Errorf("status = %#v, want completed", got)
	}
	if got := record["asr_mode"]; got != "realtime" {
		t.Errorf("asr_mode = %#v, want realtime", got)
	}
	encoded := output.String()
	for _, forbidden := range []string{"transcript", "answer", "api_key", "workspace_id", `"pcm"`} {
		if strings.Contains(strings.ToLower(encoded), forbidden) {
			t.Errorf("timing log contains forbidden field %q: %s", forbidden, encoded)
		}
	}
}

func TestEmitErrorLogsOnlySafeClassification(t *testing.T) {
	var output bytes.Buffer
	logger := slog.New(slog.NewJSONHandler(&output, nil))
	original := errors.New("provider echoed api-secret workspace-secret private transcript")
	session := &Session{
		config: Config{
			APIKey:      "api-secret",
			WorkspaceID: "workspace-secret",
			Logger:      logger,
		},
		ctx:    context.Background(),
		events: make(chan backend.ConversationEvent, 1),
	}

	session.emitError(context.Background(), "response-safe", "asr", original)

	var record map[string]any
	if err := json.Unmarshal(output.Bytes(), &record); err != nil {
		t.Fatalf("decode provider error log: %v; output=%q", err, output.String())
	}
	wantStrings := map[string]string{
		"component":   "qwen_pipeline",
		"response_id": "response-safe",
		"stage":       "asr",
		"error_code":  "provider_request_failed",
	}
	for key, want := range wantStrings {
		if got := record[key]; got != want {
			t.Errorf("%s = %#v, want %q", key, got, want)
		}
	}
	encoded := strings.ToLower(output.String())
	for _, forbidden := range []string{"api-secret", "workspace-secret", "private transcript"} {
		if strings.Contains(encoded, forbidden) {
			t.Errorf("provider error log contains forbidden value %q: %s", forbidden, output.String())
		}
	}
	if _, exists := record["error"]; exists {
		t.Errorf("provider error log contains raw error field: %s", output.String())
	}
	select {
	case event := <-session.events:
		if event.Type != backend.EventError || !errors.Is(event.Err, original) {
			t.Fatalf("error event = %+v, want original provider error", event)
		}
	default:
		t.Fatal("emitError did not publish an error event")
	}
}

func TestDiscardLastResponseKeepsPreviousPairWhileCurrentResponseIsIncomplete(t *testing.T) {
	session := &Session{history: []chatMessage{
		{Role: "user", Content: "previous"},
		{Role: "assistant", Content: "previous answer"},
	}, lastResponseDiscardable: true}
	if err := session.SendAudio(context.Background(), []byte{0, 0}); err != nil {
		t.Fatal(err)
	}
	if err := session.DiscardLastResponse(context.Background()); err != nil {
		t.Fatal(err)
	}
	if len(session.history) != 2 || session.history[0].Content != "previous" ||
		session.history[1].Content != "previous answer" {
		t.Fatalf("incomplete response removed previous history = %#v", session.history)
	}
}

func TestDiscardLastResponseRemovesOnlyMarkedCompletedPair(t *testing.T) {
	session := &Session{history: []chatMessage{
		{Role: "user", Content: "first"},
		{Role: "assistant", Content: "answer"},
		{Role: "user", Content: "completed"},
		{Role: "assistant", Content: "completed answer"},
	}, lastResponseDiscardable: true}
	if err := session.DiscardLastResponse(context.Background()); err != nil {
		t.Fatal(err)
	}
	if len(session.history) != 2 || session.history[0].Content != "first" ||
		session.history[1].Content != "answer" {
		t.Fatalf("history after discard = %#v", session.history)
	}
	if session.lastResponseDiscardable {
		t.Fatal("discardable marker remained set after deletion")
	}
	if err := session.DiscardLastResponse(context.Background()); err != nil {
		t.Fatal(err)
	}
	if len(session.history) != 2 {
		t.Fatalf("second discard removed an older pair = %#v", session.history)
	}
}

func TestCancelRemovesResponseThatFinishedBeforePlaybackBargeIn(t *testing.T) {
	session := &Session{
		history: []chatMessage{
			{Role: "user", Content: "heard"},
			{Role: "assistant", Content: "heard answer"},
			{Role: "user", Content: "interrupted"},
			{Role: "assistant", Content: "generated but not fully played"},
		},
		lastResponseDiscardable: true,
	}
	if err := session.Cancel(context.Background()); err != nil {
		t.Fatal(err)
	}
	if len(session.history) != 2 || session.history[0].Content != "heard" ||
		session.history[1].Content != "heard answer" {
		t.Fatalf("history after playback barge-in = %#v", session.history)
	}
}

func TestCancelDuringNewInputKeepsPreviousCompletedResponse(t *testing.T) {
	session := &Session{
		pcm: []byte{0, 0},
		history: []chatMessage{
			{Role: "user", Content: "heard"},
			{Role: "assistant", Content: "heard answer"},
		},
		lastResponseDiscardable: true,
	}
	if err := session.Cancel(context.Background()); err != nil {
		t.Fatal(err)
	}
	if len(session.history) != 2 {
		t.Fatalf("history after input cancellation = %#v", session.history)
	}
}

func TestBoundedHistoryKeepsNewestTurnWithoutEndingSession(t *testing.T) {
	history := make([]chatMessage, 0, 20)
	for index := 0; index < 10; index++ {
		history = append(history,
			chatMessage{Role: "user", Content: strings.Repeat("旧", 100)},
			chatMessage{Role: "assistant", Content: strings.Repeat("答", 100)})
	}
	bounded := boundedHistory(history, 3, 1024, "short prompt")
	if len(bounded) > 6 {
		t.Fatalf("bounded history has %d messages, want at most 6", len(bounded))
	}
	if len(bounded) == 0 || bounded[len(bounded)-1].Content != strings.Repeat("答", 100) {
		t.Fatalf("bounded history did not preserve newest response: %#v", bounded)
	}
	if estimateHistoryTokens(bounded) > 1024 {
		t.Fatalf("bounded history estimate = %d, want <= 1024", estimateHistoryTokens(bounded))
	}
}

func TestBoundedHistoryDoesNotStartWithOrphanAssistant(t *testing.T) {
	history := []chatMessage{
		{Role: "user", Content: "u1"},
		{Role: "assistant", Content: "a1"},
		{Role: "user", Content: "u2"},
		{Role: "assistant", Content: "a2"},
		{Role: "user", Content: "u3"},
	}
	bounded := boundedHistory(history, 2, 4096, "prompt")
	if len(bounded) != 3 || bounded[0].Role != "user" ||
		bounded[0].Content != "u2" || bounded[2].Content != "u3" {
		t.Fatalf("bounded in-progress history = %#v", bounded)
	}
}

type recordingTTSSynthesizer struct {
	text  strings.Builder
	pcm   []byte
	err   error
	calls int
}

func (s *recordingTTSSynthesizer) synthesizeStream(
	_ context.Context,
	fragments <-chan string,
	emit func([]byte) error,
) error {
	s.calls++
	for fragment := range fragments {
		s.text.WriteString(fragment)
	}
	if s.err != nil {
		return s.err
	}
	return emit(s.pcm)
}

func TestClearConversationCommandClearsHistoryAndConfirmsLocally(t *testing.T) {
	synthesizer := &recordingTTSSynthesizer{pcm: []byte{1, 2, 3, 4}}
	session := &Session{
		tts:    synthesizer,
		ctx:    context.Background(),
		events: make(chan backend.ConversationEvent, 3),
		history: []chatMessage{
			{Role: "user", Content: "旧问题"},
			{Role: "assistant", Content: "旧回答"},
		},
		lastResponseDiscardable: true,
	}
	timing := newTurnTiming(nil, "response-clear", time.Now(), 640)

	handled, err := session.handleClearConversation(
		context.Background(), "response-clear", " 清空对话。 ", timing,
	)
	if err != nil || !handled {
		t.Fatalf("handleClearConversation() = %t, %v", handled, err)
	}
	if len(session.history) != 0 || session.lastResponseDiscardable {
		t.Fatalf("history was not reset = %#v, discardable=%t",
			session.history, session.lastResponseDiscardable)
	}
	if synthesizer.calls != 1 || synthesizer.text.String() != clearConversationConfirmation {
		t.Fatalf("TTS calls/text = %d/%q", synthesizer.calls, synthesizer.text.String())
	}

	wantTypes := []backend.EventType{backend.EventTextDelta, backend.EventAudio, backend.EventDone}
	for index, wantType := range wantTypes {
		event := <-session.events
		if event.Type != wantType || event.ResponseID != "response-clear" {
			t.Fatalf("event %d = %+v, want type %d for response-clear", index, event, wantType)
		}
		if event.Type == backend.EventTextDelta && event.Text != clearConversationConfirmation {
			t.Fatalf("confirmation text = %q", event.Text)
		}
		if event.Type == backend.EventAudio &&
			(event.SampleRateHz != 24000 || !bytes.Equal(event.PCM, synthesizer.pcm)) {
			t.Fatalf("confirmation audio = rate %d pcm %v", event.SampleRateHz, event.PCM)
		}
	}
}

func TestClearConversationCommandKeepsHistoryClearedWhenConfirmationFails(t *testing.T) {
	synthesizer := &recordingTTSSynthesizer{err: errors.New("TTS unavailable")}
	session := &Session{
		tts:     synthesizer,
		ctx:     context.Background(),
		events:  make(chan backend.ConversationEvent, 1),
		history: []chatMessage{{Role: "user", Content: "must disappear"}},
	}

	handled, err := session.handleClearConversation(
		context.Background(), "response-clear", "清空对话", newTurnTiming(nil, "response-clear", time.Now(), 640),
	)
	if !handled || err == nil || !strings.Contains(err.Error(), "TTS unavailable") {
		t.Fatalf("handleClearConversation() = %t, %v", handled, err)
	}
	if len(session.history) != 0 {
		t.Fatalf("history survived failed confirmation = %#v", session.history)
	}
}

func TestTTSAudioIsPublishedAsBounded20msEvents(t *testing.T) {
	pcm := make([]byte, ttsEventPCMBytes*2+200)
	for index := range pcm {
		pcm[index] = byte(index)
	}
	session := &Session{
		ctx:    context.Background(),
		events: make(chan backend.ConversationEvent, 3),
	}
	if err := session.emitTTSAudio(
		context.Background(), "response-bounded",
		newTurnTiming(nil, "response-bounded", time.Now(), 640), pcm,
	); err != nil {
		t.Fatalf("emitTTSAudio() error = %v", err)
	}

	var rebuilt []byte
	for index, wantBytes := range []int{ttsEventPCMBytes, ttsEventPCMBytes, 200} {
		event := <-session.events
		if event.Type != backend.EventAudio || event.ResponseID != "response-bounded" ||
			event.SampleRateHz != ttsSampleRateHz || len(event.PCM) != wantBytes {
			t.Fatalf("audio event %d = %+v, want %d bytes", index, event, wantBytes)
		}
		rebuilt = append(rebuilt, event.PCM...)
	}
	if !bytes.Equal(rebuilt, pcm) {
		t.Fatal("bounded audio events changed PCM")
	}
}

func TestClearConversationCommandMatching(t *testing.T) {
	for _, testCase := range []struct {
		transcript string
		want       bool
	}{
		{transcript: "清空对话", want: true},
		{transcript: " 清空 对话。", want: true},
		{transcript: "“清空对话！”", want: true},
		{transcript: "请清空对话", want: false},
		{transcript: "清空对话记录", want: false},
		{transcript: "", want: false},
	} {
		if got := isClearConversationCommand(testCase.transcript); got != testCase.want {
			t.Errorf("isClearConversationCommand(%q) = %t, want %t",
				testCase.transcript, got, testCase.want)
		}
	}
}
