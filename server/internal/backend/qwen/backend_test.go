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

func TestSessionHandshakeCommandsAndEventMapping(t *testing.T) {
	commands := make(chan clientEvent, 5)
	endpoint := startProvider(t, func(conn *websocket.Conn) {
		writeServerJSON(t, conn, map[string]any{"type": "session.created"})
		var update clientEvent
		readClientJSON(t, conn, &update)
		commands <- update
		writeServerJSON(t, conn, map[string]any{"type": "session.updated"})

		for range 4 {
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
		writeServerJSON(t, conn, map[string]any{
			"type":     "response.done",
			"response": map[string]any{"id": "resp-1", "status": "completed"},
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
	session := opened.(*Session)
	defer func() {
		if err := session.Close(); err != nil {
			t.Errorf("Close() error = %v", err)
		}
	}()

	pcm := []byte{0x01, 0x02, 0x03, 0x04}
	if err := session.SendAudio(context.Background(), pcm); err != nil {
		t.Fatalf("SendAudio() error = %v", err)
	}
	if err := session.Commit(context.Background()); err != nil {
		t.Fatalf("Commit() error = %v", err)
	}
	if err := session.Cancel(context.Background()); err != nil {
		t.Fatalf("Cancel() error = %v", err)
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
		"response.cancel",
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
		{Type: backend.EventDone, ResponseID: "resp-1"},
	}
	for _, want := range wantEvents {
		got := receive(t, session.Events())
		if got.Type != want.Type || got.ResponseID != want.ResponseID || got.Text != want.Text || got.SampleRateHz != want.SampleRateHz || string(got.PCM) != string(want.PCM) {
			t.Fatalf("event = %#v, want %#v", got, want)
		}
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
	session := opened.(*Session)
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

func TestProviderErrorDuringHandshakeFailsOpen(t *testing.T) {
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
	_, err = b.Open(context.Background(), backend.SessionConfig{})
	if !errors.Is(err, ErrProvider) {
		t.Fatalf("Open() error = %v, want provider error", err)
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
	_, err = b.Open(context.Background(), backend.SessionConfig{})
	if !errors.Is(err, ErrTransport) {
		t.Fatalf("Open() error = %v, want transport error", err)
	}
	if !strings.Contains(err.Error(), "401 Unauthorized") {
		t.Fatalf("Open() error = %v, want HTTP response status", err)
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
	_, err = b.Open(context.Background(), backend.SessionConfig{})
	if !errors.Is(err, ErrTransport) {
		t.Fatalf("Open() error = %v, want transport error", err)
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

func TestBoundedSendQueueClassifiesDeadline(t *testing.T) {
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	session := &Session{
		config: Config{},
		ctx:    ctx,
		cancel: cancel,
		writes: make(chan outboundBatch, 1),
	}
	session.writes <- outboundBatch{}
	callCtx, stop := context.WithTimeout(context.Background(), time.Millisecond)
	defer stop()
	err := session.SendAudio(callCtx, []byte{1, 2})
	if !errors.Is(err, ErrTransport) || !errors.Is(err, context.DeadlineExceeded) {
		t.Fatalf("SendAudio() error = %v, want transport deadline", err)
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
	session := opened.(*Session)
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
