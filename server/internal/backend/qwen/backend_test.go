package qwen

import (
	"context"
	"encoding/base64"
	"encoding/json"
	"errors"
	"net/http"
	"net/http/httptest"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/gorilla/websocket"
	"github.com/lst92991-eng/boomPI/server/internal/backend"
)

func TestOpenReturnsBeforeProviderSessionIsReady(t *testing.T) {
	updateReceived := make(chan struct{})
	releaseHandshake := make(chan struct{})
	endpoint := startProvider(t, func(conn *websocket.Conn) {
		writeServerJSON(t, conn, map[string]any{"type": "session.created"})
		var update clientEvent
		readClientJSON(t, conn, &update)
		close(updateReceived)
		<-releaseHandshake
		writeServerJSON(t, conn, map[string]any{"type": "session.updated"})
		for {
			if _, _, err := conn.ReadMessage(); err != nil {
				return
			}
		}
	})
	b, err := New(validConfig(endpoint))
	if err != nil {
		t.Fatalf("New() error = %v", err)
	}

	started := time.Now()
	session, err := b.Open(context.Background(), backend.SessionConfig{})
	if err != nil {
		t.Fatalf("Open() error = %v", err)
	}
	defer session.Close()
	if elapsed := time.Since(started); elapsed > 100*time.Millisecond {
		t.Fatalf("Open() waited %s for provider readiness", elapsed)
	}
	select {
	case <-updateReceived:
	case <-time.After(time.Second):
		t.Fatal("provider did not receive session.update")
	}

	sendResult := make(chan error, 1)
	go func() { sendResult <- session.SendAudio(context.Background(), []byte{1, 2}) }()
	select {
	case err := <-sendResult:
		t.Fatalf("SendAudio() returned before session.updated: %v", err)
	case <-time.After(25 * time.Millisecond):
	}
	close(releaseHandshake)
	select {
	case err := <-sendResult:
		if err != nil {
			t.Fatalf("SendAudio() after session.updated error = %v", err)
		}
	case <-time.After(time.Second):
		t.Fatal("SendAudio() remained blocked after session.updated")
	}
}

func TestSessionHandshakeCommandsAndEventMapping(t *testing.T) {
	commands := make(chan clientEvent, 5)
	endpoint := startProvider(t, func(conn *websocket.Conn) {
		writeServerJSON(t, conn, map[string]any{"type": "session.created"})
		var update clientEvent
		readClientJSON(t, conn, &update)
		commands <- update
		writeServerJSON(t, conn, map[string]any{"type": "session.updated"})

		for range 3 {
			var command clientEvent
			readClientJSON(t, conn, &command)
			commands <- command
		}
		writeServerJSON(t, conn, map[string]any{
			"type":     "response.created",
			"response": map[string]any{"id": "resp-1", "status": "in_progress"},
		})
		writeServerJSON(t, conn, map[string]any{
			"type": "response.audio_transcript.delta", "response_id": "resp-1", "delta": "你好",
		})
		writeServerJSON(t, conn, map[string]any{
			"type": "response.audio.delta", "response_id": "resp-1", "delta": base64.StdEncoding.EncodeToString([]byte{1, 2, 3, 4}),
		})
		var cancelCommand clientEvent
		readClientJSON(t, conn, &cancelCommand)
		commands <- cancelCommand
		writeServerJSON(t, conn, map[string]any{
			"type":     "response.done",
			"response": map[string]any{"id": "resp-1", "status": "cancelled"},
		})
	})

	b, err := New(validConfig(endpoint))
	if err != nil {
		t.Fatalf("New() error = %v", err)
	}
	opened, err := b.Open(context.Background(), backend.SessionConfig{
		SystemPrompt: "Answer briefly.",
		Persona:      "Use Chinese.",
	})
	if err != nil {
		t.Fatalf("Open() error = %v", err)
	}
	session := opened
	defer func() {
		if err := session.Close(); err != nil {
			t.Errorf("Close() error = %v", err)
		}
	}()

	pcm := []byte{0x01, 0x02, 0x03, 0x04}
	if err := sendAudioWhenReady(t, session, pcm); err != nil {
		t.Fatalf("SendAudio() error = %v", err)
	}
	if err := session.Commit(context.Background()); err != nil {
		t.Fatalf("Commit() error = %v", err)
	}

	update := receive(t, commands)
	if update.Type != "session.update" || update.Session == nil {
		t.Fatalf("session update = %#v", update)
	}
	if update.Session.TurnDetection != nil {
		t.Fatalf("turn_detection = %#v, want nil manual mode", update.Session.TurnDetection)
	}
	if update.Session.Instructions != "Answer briefly.\n\nUse Chinese." {
		t.Fatalf("instructions = %q", update.Session.Instructions)
	}

	wantTypes := []string{
		"input_audio_buffer.append",
		"input_audio_buffer.commit",
		"response.create",
	}
	for _, want := range wantTypes {
		command := receive(t, commands)
		if command.Type != want {
			t.Fatalf("command type = %q, want %q", command.Type, want)
		}
		if want == "input_audio_buffer.append" {
			decoded, decodeErr := base64.StdEncoding.DecodeString(command.Audio)
			if decodeErr != nil || string(decoded) != string(pcm) {
				t.Fatalf("append audio = %q, decode error = %v", command.Audio, decodeErr)
			}
		}
	}

	wantEvents := []backend.ConversationEvent{
		{Type: backend.EventStarted, ResponseID: "resp-1"},
		{Type: backend.EventTextDelta, ResponseID: "resp-1", Text: "你好"},
		{Type: backend.EventAudio, ResponseID: "resp-1", PCM: []byte{1, 2, 3, 4}, SampleRateHz: 24_000},
	}
	for _, want := range wantEvents {
		got := receive(t, session.Events())
		if got.Type != want.Type || got.ResponseID != want.ResponseID || got.Text != want.Text || got.SampleRateHz != want.SampleRateHz || string(got.PCM) != string(want.PCM) {
			t.Fatalf("event = %#v, want %#v", got, want)
		}
	}
	if err := session.Cancel(context.Background()); err != nil {
		t.Fatalf("Cancel() error = %v", err)
	}
	if command := receive(t, commands); command.Type != "response.cancel" {
		t.Fatalf("command type = %q, want response.cancel", command.Type)
	}
	select {
	case stale, ok := <-session.Events():
		if ok {
			t.Fatalf("cancelled response leaked a stale event: %#v", stale)
		}
	case <-time.After(25 * time.Millisecond):
	}
}

func TestProviderErrorAfterHandshakeIsAnEvent(t *testing.T) {
	endpoint := startProvider(t, func(conn *websocket.Conn) {
		completeHandshake(t, conn)
		writeServerJSON(t, conn, map[string]any{
			"type":  "error",
			"error": map[string]any{"type": "invalid_request_error", "code": "invalid_value", "message": "bad request"},
		})
		<-time.After(100 * time.Millisecond)
	})
	b, err := New(validConfig(endpoint))
	if err != nil {
		t.Fatalf("New() error = %v", err)
	}
	opened, err := b.Open(context.Background(), backend.SessionConfig{})
	if err != nil {
		t.Fatalf("Open() error = %v", err)
	}
	session := opened
	defer session.Close()
	event := receive(t, session.Events())
	if event.Type != backend.EventError || !errors.Is(event.Err, ErrProvider) {
		t.Fatalf("event = %#v", event)
	}
}

func TestResponseDoneStatusMapping(t *testing.T) {
	tests := []struct {
		status  string
		want    backend.EventType
		wantErr bool
	}{
		{status: "completed", want: backend.EventDone},
		{status: "cancelled", want: backend.EventDone},
		{status: "failed", want: backend.EventError, wantErr: true},
		{status: "incomplete", want: backend.EventError, wantErr: true},
		{status: "in_progress", want: backend.EventError, wantErr: true},
	}
	for _, test := range tests {
		t.Run(test.status, func(t *testing.T) {
			session := &Session{events: make(chan backend.ConversationEvent, 1)}
			var event serverEvent
			event.Type = "response.done"
			event.Response.ID = "resp-1"
			event.Response.Status = test.status
			if err := session.handleServerEvent(event); err != nil {
				t.Fatalf("handleServerEvent() error = %v", err)
			}
			got := receive(t, session.events)
			if got.Type != test.want || got.ResponseID != "resp-1" {
				t.Fatalf("event = %#v, want type %d", got, test.want)
			}
			if (got.Err != nil) != test.wantErr {
				t.Fatalf("event error = %v, wantErr=%v", got.Err, test.wantErr)
			}
		})
	}
}

func TestProviderErrorDuringHandshakeFailsFirstOperation(t *testing.T) {
	endpoint := startProvider(t, func(conn *websocket.Conn) {
		writeServerJSON(t, conn, map[string]any{"type": "session.created"})
		var update clientEvent
		readClientJSON(t, conn, &update)
		writeServerJSON(t, conn, map[string]any{
			"type":  "error",
			"error": map[string]any{"type": "invalid_request_error", "code": "invalid_value", "message": "invalid voice"},
		})
	})
	b, err := New(validConfig(endpoint))
	if err != nil {
		t.Fatalf("New() error = %v", err)
	}
	opened, err := b.Open(context.Background(), backend.SessionConfig{})
	if err != nil {
		t.Fatalf("Open() error = %v", err)
	}
	defer opened.Close()
	err = awaitSessionOperationError(t, opened)
	if !errors.Is(err, ErrProvider) {
		t.Fatalf("SendAudio() error = %v, want provider error", err)
	}
}

func TestAuthenticationFailureIsClassified(t *testing.T) {
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, _ *http.Request) {
		http.Error(w, "unauthorized", http.StatusUnauthorized)
	}))
	defer server.Close()
	b, err := New(validConfig("ws" + strings.TrimPrefix(server.URL, "http")))
	if err != nil {
		t.Fatalf("New() error = %v", err)
	}
	opened, err := b.Open(context.Background(), backend.SessionConfig{})
	if err != nil {
		t.Fatalf("Open() error = %v", err)
	}
	defer opened.Close()
	err = awaitSessionOperationError(t, opened)
	if !errors.Is(err, ErrTransport) {
		t.Fatalf("SendAudio() error = %v, want transport error", err)
	}
	if !strings.Contains(err.Error(), "401 Unauthorized") {
		t.Fatalf("SendAudio() error = %v, want HTTP response status", err)
	}
}

func TestSafeHTTPStatusCannotReflectRemoteReasonPhrase(t *testing.T) {
	const apiKey = "test-api-key"
	response := http.Response{StatusCode: http.StatusUnauthorized, Status: "401 " + apiKey}
	status := safeHTTPStatus(response.StatusCode)
	if status != "401 Unauthorized" {
		t.Fatalf("safeHTTPStatus() = %q", status)
	}
	if strings.Contains(status, apiKey) {
		t.Fatalf("safeHTTPStatus() reflected remote data: %q", status)
	}
}

func TestHandshakeDiagnosticRedactsAPIKey(t *testing.T) {
	const apiKey = "test-api-key"
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, _ *http.Request) {
		w.Header().Set("Content-Type", "application/json")
		w.WriteHeader(http.StatusUnauthorized)
		_, _ = w.Write([]byte(`{"code":"InvalidApiKey-test-api-key","message":"credential test-api-key was rejected"}`))
	}))
	defer server.Close()

	cfg := validConfig("ws" + strings.TrimPrefix(server.URL, "http"))
	cfg.APIKey = apiKey
	b, err := New(cfg)
	if err != nil {
		t.Fatalf("New() error = %v", err)
	}
	opened, err := b.Open(context.Background(), backend.SessionConfig{})
	if err != nil {
		t.Fatalf("Open() error = %v", err)
	}
	defer opened.Close()
	err = awaitSessionOperationError(t, opened)
	if !errors.Is(err, ErrTransport) {
		t.Fatalf("SendAudio() error = %v, want transport error", err)
	}
	message := err.Error()
	if !strings.Contains(message, "InvalidApiKey") || !strings.Contains(message, "<redacted>") {
		t.Fatalf("Open() error = %v, want redacted structured diagnostic", err)
	}
	if strings.Contains(message, apiKey) {
		t.Fatalf("Open() error leaked API key: %v", err)
	}
}

func TestBoundedSendQueueHonorsCancellation(t *testing.T) {
	ctx, cancel := context.WithCancel(context.Background())
	session := &Session{
		config: Config{},
		ctx:    ctx,
		cancel: cancel,
		writes: make(chan outboundBatch, 1),
	}
	session.writes <- outboundBatch{}
	callCtx, stop := context.WithCancel(context.Background())
	stop()
	err := session.SendAudio(callCtx, []byte{1, 2})
	if !errors.Is(err, ErrCanceled) {
		t.Fatalf("SendAudio() error = %v, want canceled", err)
	}
}

func TestBoundedAudioQueueReturnsBackpressureWithoutBlockingActor(t *testing.T) {
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	session := &Session{
		config: Config{},
		ctx:    ctx,
		cancel: cancel,
		writes: make(chan outboundBatch, 1),
	}
	session.writes <- outboundBatch{}
	started := time.Now()
	err := session.SendAudio(context.Background(), []byte{1, 2})
	if !errors.Is(err, ErrBackpressure) {
		t.Fatalf("SendAudio() error = %v, want backpressure", err)
	}
	if elapsed := time.Since(started); elapsed > 100*time.Millisecond {
		t.Fatalf("SendAudio() waited %s on a full provider queue", elapsed)
	}
}

func TestListeningCancelBypassesFullAudioQueue(t *testing.T) {
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	session := &Session{
		config: Config{}, ctx: ctx, cancel: cancel,
		writes: make(chan outboundBatch, 1), urgentWrites: make(chan outboundBatch, 1),
	}
	session.writes <- outboundBatch{
		events: []clientEvent{{Type: "input_audio_buffer.append"}}, inputAudio: true,
	}
	if err := session.Cancel(context.Background()); err != nil {
		t.Fatalf("Cancel() with full audio queue error = %v", err)
	}
	select {
	case batch := <-session.urgentWrites:
		if len(batch.events) != 1 || batch.events[0].Type != "input_audio_buffer.clear" {
			t.Fatalf("urgent cancel batch = %+v", batch)
		}
	case <-time.After(time.Second):
		t.Fatal("listening cancel did not bypass the full audio queue")
	}
	if got := session.inputGeneration.Load(); got != 1 {
		t.Fatalf("input generation after cancel = %d, want 1", got)
	}
}

func TestCommitReturnsBackpressureAndRollsBackResponseState(t *testing.T) {
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	session := &Session{ctx: ctx, cancel: cancel, writes: make(chan outboundBatch, 1)}
	session.writes <- outboundBatch{}
	if err := session.Commit(context.Background()); !errors.Is(err, ErrBackpressure) {
		t.Fatalf("Commit() error = %v, want backpressure", err)
	}
	if session.responseRequested {
		t.Fatal("Commit() left response active after queue backpressure")
	}
}

func TestCancelDoesNotHoldResponseLockWhileQueueIsFull(t *testing.T) {
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	session := &Session{
		config:            Config{Timeout: time.Second},
		ctx:               ctx,
		cancel:            cancel,
		writes:            make(chan outboundBatch, 1),
		responseRequested: true,
	}
	session.writes <- outboundBatch{}
	callCtx, stopCall := context.WithCancel(context.Background())
	done := make(chan error, 1)
	go func() { done <- session.Cancel(callCtx) }()

	deadline := time.Now().Add(500 * time.Millisecond)
	for {
		if session.responseMu.TryLock() {
			waiting := session.cancelWait != nil
			session.responseMu.Unlock()
			if waiting {
				break
			}
		}
		if time.Now().After(deadline) {
			t.Fatal("Cancel held responseMu while waiting for queue capacity")
		}
		time.Sleep(time.Millisecond)
	}
	stopCall()
	if err := <-done; !errors.Is(err, ErrCanceled) {
		t.Fatalf("Cancel() error = %v, want canceled", err)
	}
}

func TestCloseStopsWorkersAndRejectsNewCommands(t *testing.T) {
	endpoint := startProvider(t, func(conn *websocket.Conn) {
		completeHandshake(t, conn)
		for {
			if _, _, err := conn.ReadMessage(); err != nil {
				return
			}
		}
	})
	b, err := New(validConfig(endpoint))
	if err != nil {
		t.Fatalf("New() error = %v", err)
	}
	opened, err := b.Open(context.Background(), backend.SessionConfig{})
	if err != nil {
		t.Fatalf("Open() error = %v", err)
	}
	session := opened
	if err := session.Close(); err != nil {
		t.Fatalf("Close() error = %v", err)
	}
	if err := session.SendAudio(context.Background(), []byte{1, 2}); !errors.Is(err, ErrSessionClosed) {
		t.Fatalf("SendAudio() after close error = %v", err)
	}
	if _, ok := <-session.Events(); ok {
		t.Fatal("Events() remained open after Close()")
	}
}

func startProvider(t *testing.T, run func(*websocket.Conn)) string {
	t.Helper()
	upgrader := websocket.Upgrader{CheckOrigin: func(*http.Request) bool { return true }}
	var wg sync.WaitGroup
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.Header.Get("Authorization") != "Bearer test-api-key" {
			t.Error("Authorization header mismatch")
		}
		if r.URL.Query().Get("model") != "qwen3.5-omni-plus-realtime" {
			t.Errorf("model query = %q", r.URL.Query().Get("model"))
		}
		conn, err := upgrader.Upgrade(w, r, nil)
		if err != nil {
			t.Errorf("Upgrade() error = %v", err)
			return
		}
		wg.Add(1)
		defer wg.Done()
		defer conn.Close()
		run(conn)
	}))
	t.Cleanup(func() {
		server.Close()
		wg.Wait()
	})
	return "ws" + strings.TrimPrefix(server.URL, "http") + "/api-ws/v1/realtime"
}

func completeHandshake(t *testing.T, conn *websocket.Conn) {
	t.Helper()
	writeServerJSON(t, conn, map[string]any{"type": "session.created"})
	var update clientEvent
	readClientJSON(t, conn, &update)
	if update.Type != "session.update" {
		t.Errorf("first client event = %q", update.Type)
	}
	writeServerJSON(t, conn, map[string]any{"type": "session.updated"})
}

func writeServerJSON(t *testing.T, conn *websocket.Conn, value any) {
	t.Helper()
	if err := conn.WriteJSON(value); err != nil {
		t.Errorf("provider WriteJSON() error = %v", err)
	}
}

func readClientJSON(t *testing.T, conn *websocket.Conn, target any) {
	t.Helper()
	if err := conn.ReadJSON(target); err != nil {
		t.Errorf("provider ReadJSON() error = %v", err)
	}
}

func receive[T any](t *testing.T, channel <-chan T) T {
	t.Helper()
	select {
	case value, ok := <-channel:
		if !ok {
			t.Fatal("channel closed before value arrived")
		}
		return value
	case <-time.After(2 * time.Second):
		t.Fatal("timed out waiting for channel value")
		var zero T
		return zero
	}
}

func awaitSessionOperationError(t *testing.T, session backend.ConversationSession) error {
	t.Helper()
	deadline := time.Now().Add(time.Second)
	for {
		err := session.SendAudio(context.Background(), []byte{1, 2})
		if !errors.Is(err, ErrSessionNotReady) {
			return err
		}
		if time.Now().After(deadline) {
			t.Fatal("timed out waiting for provider initialization error")
		}
		time.Sleep(time.Millisecond)
	}
}

func sendAudioWhenReady(t *testing.T, session backend.ConversationSession, pcm []byte) error {
	t.Helper()
	deadline := time.Now().Add(time.Second)
	for {
		err := session.SendAudio(context.Background(), pcm)
		if !errors.Is(err, ErrSessionNotReady) {
			return err
		}
		if time.Now().After(deadline) {
			t.Fatal("timed out waiting for provider session readiness")
		}
		time.Sleep(time.Millisecond)
	}
}

func TestInvalidAudioIsRejectedBeforeQueueing(t *testing.T) {
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	session := &Session{config: Config{}, ctx: ctx, writes: make(chan outboundBatch, 1)}
	for _, pcm := range [][]byte{nil, {1}, {1, 2, 3}, make([]byte, maxAudioChunkBytes+2)} {
		err := session.SendAudio(context.Background(), pcm)
		if err == nil {
			t.Fatalf("SendAudio(%d bytes) unexpectedly succeeded", len(pcm))
		}
	}
}

func TestProviderAudioIsPublishedAsBounded20msEvents(t *testing.T) {
	pcm := make([]byte, outputAudioEventBytes*2+200)
	for index := range pcm {
		pcm[index] = byte(index)
	}
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	session := &Session{ctx: ctx, events: make(chan backend.ConversationEvent, 3)}
	event := serverEvent{Type: "response.audio.delta", ResponseID: "response-bounded"}
	event.Delta = base64.StdEncoding.EncodeToString(pcm)
	if err := session.handleServerEvent(event); err != nil {
		t.Fatalf("handleServerEvent() error = %v", err)
	}

	var rebuilt []byte
	for index, wantBytes := range []int{outputAudioEventBytes, outputAudioEventBytes, 200} {
		event := receive(t, session.Events())
		if event.Type != backend.EventAudio || event.ResponseID != "response-bounded" ||
			event.SampleRateHz != 24_000 || len(event.PCM) != wantBytes {
			t.Fatalf("audio event %d = %+v, want %d bytes", index, event, wantBytes)
		}
		rebuilt = append(rebuilt, event.PCM...)
	}
	if string(rebuilt) != string(pcm) {
		t.Fatal("bounded audio events changed PCM")
	}
}

func TestProviderAudioBackpressurePreservesDeltaLargerThanQueue(t *testing.T) {
	const queueSize = 2
	pcm := make([]byte, outputAudioEventBytes*5+200)
	for index := range pcm {
		pcm[index] = byte(index)
	}
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	session := &Session{ctx: ctx, events: make(chan backend.ConversationEvent, queueSize)}
	event := serverEvent{Type: "response.audio.delta", ResponseID: "response-backpressure"}
	event.Delta = base64.StdEncoding.EncodeToString(pcm)
	result := make(chan error, 1)
	go func() { result <- session.handleServerEvent(event) }()

	select {
	case err := <-result:
		t.Fatalf("large provider delta bypassed bounded backpressure: %v", err)
	case <-time.After(25 * time.Millisecond):
	}

	var rebuilt []byte
	for index := 0; index < 6; index++ {
		published := receive(t, session.Events())
		if published.Type != backend.EventAudio || published.ResponseID != event.ResponseID {
			t.Fatalf("audio event %d = %+v", index, published)
		}
		rebuilt = append(rebuilt, published.PCM...)
		time.Sleep(time.Millisecond)
	}
	select {
	case err := <-result:
		if err != nil {
			t.Fatalf("large provider delta failed after the consumer caught up: %v", err)
		}
	case <-time.After(time.Second):
		t.Fatal("large provider delta remained blocked after queue space became available")
	}
	if string(rebuilt) != string(pcm) {
		t.Fatal("backpressured provider audio changed PCM")
	}
}

func TestProviderAudioBackpressureUnblocksOnSessionCancellation(t *testing.T) {
	ctx, cancel := context.WithCancel(context.Background())
	session := &Session{ctx: ctx, events: make(chan backend.ConversationEvent, 1)}
	pcm := make([]byte, outputAudioEventBytes*2)
	event := serverEvent{Type: "response.audio.delta", ResponseID: "response-cancel"}
	event.Delta = base64.StdEncoding.EncodeToString(pcm)
	result := make(chan error, 1)
	go func() { result <- session.handleServerEvent(event) }()

	select {
	case err := <-result:
		t.Fatalf("provider publish returned before its full queue was canceled: %v", err)
	case <-time.After(25 * time.Millisecond):
	}
	cancel()
	select {
	case err := <-result:
		if !errors.Is(err, context.Canceled) {
			t.Fatalf("canceled provider publish error = %v, want context.Canceled", err)
		}
	case <-time.After(time.Second):
		t.Fatal("provider publish remained blocked after session cancellation")
	}
}

func TestProviderAudioBackpressureUnblocksOnTurnCancellation(t *testing.T) {
	ctx, stopSession := context.WithCancel(context.Background())
	defer stopSession()
	session := &Session{
		config: Config{Timeout: time.Second}, ctx: ctx, cancel: stopSession,
		writes: make(chan outboundBatch, 2), events: make(chan backend.ConversationEvent, 1),
	}
	if err := session.Commit(context.Background()); err != nil {
		t.Fatalf("Commit() error = %v", err)
	}
	sentinel := backend.ConversationEvent{Type: backend.EventStarted, ResponseID: "already-queued"}
	session.events <- sentinel

	pcm := make([]byte, outputAudioEventBytes*2)
	audio := serverEvent{Type: "response.audio.delta", ResponseID: "response-cancel"}
	audio.Delta = base64.StdEncoding.EncodeToString(pcm)
	publishResult := make(chan error, 1)
	go func() { publishResult <- session.handleServerEvent(audio) }()
	select {
	case err := <-publishResult:
		t.Fatalf("provider publish returned before turn cancellation: %v", err)
	case <-time.After(25 * time.Millisecond):
	}

	cancelResult := make(chan error, 1)
	go func() { cancelResult <- session.Cancel(context.Background()) }()
	select {
	case err := <-publishResult:
		if err != nil {
			t.Fatalf("turn-cancelled provider publish error = %v", err)
		}
	case <-time.After(time.Second):
		t.Fatal("provider publish remained blocked after turn cancellation")
	}
	var done serverEvent
	done.Type = "response.done"
	done.Response.ID = audio.ResponseID
	done.Response.Status = "cancelled"
	if err := session.handleServerEvent(done); err != nil {
		t.Fatalf("cancel response.done error = %v", err)
	}
	select {
	case err := <-cancelResult:
		if err != nil {
			t.Fatalf("Cancel() error = %v", err)
		}
	case <-time.After(time.Second):
		t.Fatal("Cancel() did not observe response.done")
	}

	if queued := <-session.events; queued.ResponseID != sentinel.ResponseID {
		t.Fatalf("queued event = %+v, want sentinel", queued)
	}
	select {
	case stale := <-session.events:
		t.Fatalf("cancelled response leaked a stale event: %+v", stale)
	default:
	}
}

func TestWireSessionUpdateUsesExplicitNullTurnDetection(t *testing.T) {
	event := newSessionUpdate(validConfig("ws://localhost/realtime"), backend.SessionConfig{})
	payload, err := json.Marshal(event)
	if err != nil {
		t.Fatalf("json.Marshal() error = %v", err)
	}
	if !strings.Contains(string(payload), `"turn_detection":null`) {
		t.Fatalf("session update JSON = %s", payload)
	}
}
