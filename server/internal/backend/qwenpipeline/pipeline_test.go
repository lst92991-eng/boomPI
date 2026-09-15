package qwenpipeline

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"log/slog"
	"net"
	"net/http"
	"net/http/httptest"
	"strings"
	"sync"
	"sync/atomic"
	"testing"
	"time"

	"github.com/gorilla/websocket"
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
	inputCtx, cancelInput := context.WithTimeout(context.Background(), 30*time.Millisecond)
	defer cancelInput()
	if err := session.SendAudio(inputCtx, []byte{0, 0}); !errors.Is(err, context.DeadlineExceeded) {
		t.Fatalf("unprepared realtime ASR must fail within input deadline: %v", err)
	}
	if session.inputBytes != 0 {
		t.Fatal("failed input was counted as accepted")
	}
	if err := session.Cancel(context.Background(), false); err != nil {
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
		if event.Type != backend.EventError || event.ResponseID != "response-safe" || !errors.Is(event.Err, original) {
			t.Fatalf("error event = %+v, want original provider error", event)
		}
	default:
		t.Fatal("emitError did not publish an error event")
	}
}

func TestCancelRetractKeepsPreviousPairWhileCurrentResponseIsIncomplete(t *testing.T) {
	session := &Session{history: []chatMessage{
		{Role: "user", Content: "previous"},
		{Role: "assistant", Content: "previous answer"},
	}, lastResponseDiscardable: true}
	session.inputBytes = 2
	session.lastResponseDiscardable = false
	if err := session.Cancel(context.Background(), true); err != nil {
		t.Fatal(err)
	}
	if len(session.history) != 2 || session.history[0].Content != "previous" ||
		session.history[1].Content != "previous answer" {
		t.Fatalf("incomplete response removed previous history = %#v", session.history)
	}
}

func TestCancelRetractRemovesOnlyMarkedCompletedPair(t *testing.T) {
	session := &Session{history: []chatMessage{
		{Role: "user", Content: "first"},
		{Role: "assistant", Content: "answer"},
		{Role: "user", Content: "completed"},
		{Role: "assistant", Content: "completed answer"},
	}, lastResponseDiscardable: true}
	if err := session.Cancel(context.Background(), true); err != nil {
		t.Fatal(err)
	}
	if len(session.history) != 2 || session.history[0].Content != "first" ||
		session.history[1].Content != "answer" {
		t.Fatalf("history after discard = %#v", session.history)
	}
	if session.lastResponseDiscardable {
		t.Fatal("discardable marker remained set after deletion")
	}
	if err := session.Cancel(context.Background(), true); err != nil {
		t.Fatal(err)
	}
	if len(session.history) != 2 {
		t.Fatalf("second discard removed an older pair = %#v", session.history)
	}
}

func TestCancelPreservesCompletedResponseUntilExplicitRetraction(t *testing.T) {
	session := &Session{
		history: []chatMessage{
			{Role: "user", Content: "heard"},
			{Role: "assistant", Content: "heard answer"},
			{Role: "user", Content: "interrupted"},
			{Role: "assistant", Content: "generated but not fully played"},
		},
		lastResponseDiscardable: true,
	}
	if err := session.Cancel(context.Background(), false); err != nil {
		t.Fatal(err)
	}
	if len(session.history) != 4 {
		t.Fatalf("Cancel must preserve completed history: %#v", session.history)
	}
	if err := session.Cancel(context.Background(), true); err != nil {
		t.Fatal(err)
	}
	if len(session.history) != 2 || session.history[0].Content != "heard" ||
		session.history[1].Content != "heard answer" {
		t.Fatalf("history after playback barge-in = %#v", session.history)
	}
}

func TestCancelDuringNewInputKeepsPreviousCompletedResponse(t *testing.T) {
	session := &Session{
		inputBytes: 2,
		history: []chatMessage{
			{Role: "user", Content: "heard"},
			{Role: "assistant", Content: "heard answer"},
		},
		lastResponseDiscardable: true,
	}
	if err := session.Cancel(context.Background(), false); err != nil {
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
			(event.SampleRateHz != 16000 || !bytes.Equal(event.PCM, synthesizer.pcm)) {
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

func TestPipelineASRFailureIsReportedWithoutBatchRetry(t *testing.T) {
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) { _ = serveRealtimeASRWithResult(w, r, true) }))
	defer server.Close()
	cfg := Config{APIKey: "test-key", Region: RegionChinaBeijing, ASRModel: realtimeASRModelName, ReasoningModel: "test", ReasoningEffort: "none", TTSModel: "cosyvoice-v3-flash", TTSVoice: "longxiaochun_v3", SearchMode: "off", Timeout: time.Second, QueueSize: 8}
	provider, err := New(cfg)
	if err != nil {
		t.Fatal(err)
	}
	provider.openASR = func(ctx context.Context, c Config) (*asrRealtimeStream, error) {
		return openRealtimeASRAt(ctx, c, strings.Replace(server.URL, "http://", "ws://", 1))
	}
	ctx, cancel := context.WithTimeout(context.Background(), 2*time.Second)
	defer cancel()
	raw, err := provider.Open(ctx, backend.SessionConfig{})
	if err != nil {
		t.Fatal(err)
	}
	session := raw.(*Session)
	defer session.Close()
	session.http.client = &http.Client{Transport: rejectPipelineHTTP{t}}
	for _, frame := range [][]byte{{1, 2, 3, 4}, {5, 6, 7, 8}} {
		if err := session.SendAudio(ctx, frame); err != nil {
			t.Fatal(err)
		}
	}
	if session.inputBytes != 8 {
		t.Fatalf("accepted input byte count: %d", session.inputBytes)
	}
	if err := session.Commit(ctx); err != nil {
		t.Fatal(err)
	}
	var id string
	for {
		select {
		case event := <-session.Events():
			switch event.Type {
			case backend.EventStarted:
				id = event.ResponseID
			case backend.EventError:
				if id == "" || event.ResponseID != id {
					t.Fatal("ASR error lost response ownership")
				}
				return
			default:
				t.Fatalf("unexpected event after ASR failure: %+v", event)
			}
		case <-ctx.Done():
			t.Fatal("ASR failure did not finish the turn")
		}
	}
}

type rejectPipelineHTTP struct{ t *testing.T }

func (r rejectPipelineHTTP) RoundTrip(*http.Request) (*http.Response, error) {
	r.t.Error("ASR failure invoked HTTP fallback or LLM")
	return nil, errors.New("unexpected HTTP")
}

func TestCanceledCommitLeavesInputAvailableForCancellation(t *testing.T) {
	session := &Session{inputBytes: 640}
	ctx, cancel := context.WithCancel(context.Background())
	cancel()
	if err := session.Commit(ctx); !errors.Is(err, context.Canceled) {
		t.Fatalf("canceled END: %v", err)
	}
	if session.inputBytes != 640 || session.activeDone != nil {
		t.Fatal("canceled END started a response")
	}
}

// Keep old cloud Close in progress to expose any premature close(done).
type gatedASRCloseConn struct {
	net.Conn
	entered chan struct{}
	release <-chan struct{}
	once    sync.Once
}

func (c *gatedASRCloseConn) Close() error {
	c.once.Do(func() { close(c.entered) })
	<-c.release
	return c.Conn.Close()
}

func TestCancelBeforeStartedDeliveryPreparesASRBeforeNextInput(t *testing.T) {
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		conn, err := (&websocket.Upgrader{}).Upgrade(w, r, nil)
		if err != nil {
			return
		}
		defer conn.Close()
		for {
			if _, _, err := conn.ReadMessage(); err != nil {
				return
			}
		}
	}))
	defer server.Close()
	ctx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
	defer cancel()
	closeEntered, releaseClose := make(chan struct{}), make(chan struct{})
	var releaseOnce sync.Once
	release := func() { releaseOnce.Do(func() { close(releaseClose) }) }
	defer release()
	var opened atomic.Int32
	provider := &Backend{config: Config{Timeout: time.Second, QueueSize: 8}}
	provider.openASR = func(ctx context.Context, c Config) (*asrRealtimeStream, error) {
		dialer := *websocket.DefaultDialer
		if opened.Add(1) == 1 {
			dialer.NetDialContext = func(ctx context.Context, network, address string) (net.Conn, error) {
				conn, err := (&net.Dialer{}).DialContext(ctx, network, address)
				if err != nil {
					return nil, err
				}
				return &gatedASRCloseConn{Conn: conn, entered: closeEntered, release: releaseClose}, nil
			}
		}
		conn, _, err := dialer.DialContext(ctx, strings.Replace(server.URL, "http://", "ws://", 1), nil)
		if err != nil {
			return nil, err
		}
		return &asrRealtimeStream{config: c, connection: conn}, nil
	}
	raw, err := provider.Open(ctx, backend.SessionConfig{})
	if err != nil {
		t.Fatal(err)
	}
	session := raw.(*Session)
	defer func() { release(); session.Close() }()
	if err := session.SendAudio(ctx, []byte{1, 0}); err != nil {
		t.Fatal(err)
	}
	// No receiver: EventStarted must remain blocked until cancellation.
	for i := 0; i < cap(session.events); i++ {
		session.events <- backend.ConversationEvent{}
	}
	if err := session.Commit(ctx); err != nil {
		t.Fatal(err)
	}
	canceled := make(chan error, 1)
	go func() { canceled <- session.Cancel(ctx, false) }()
	select {
	case <-closeEntered:
	case <-ctx.Done():
		t.Fatal("old ASR did not enter cleanup")
	}
	select {
	case err := <-canceled:
		t.Fatalf("Cancel returned before old ASR cleanup: %v", err)
	case <-time.After(30 * time.Millisecond):
	}
	release()
	select {
	case err := <-canceled:
		if err != nil {
			t.Fatal(err)
		}
	case <-ctx.Done():
		t.Fatal("Cancel did not finish")
	}
	// Submit immediately when Cancel returns: either await the new preconnect or use it.
	if err := session.SendAudio(ctx, []byte{2, 0}); err != nil {
		t.Fatalf("next input lost its ASR preparation: %v", err)
	}
	if session.inputBytes != 2 {
		t.Fatalf("next input count=%d", session.inputBytes)
	}
}
