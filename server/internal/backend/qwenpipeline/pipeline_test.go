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

func TestTurnTimingWritesOneSanitizedAggregateRecord(t *testing.T) {
	var output bytes.Buffer
	logger := slog.New(slog.NewJSONHandler(&output, nil))
	committedAt := time.Date(2026, time.July, 29, 12, 0, 0, 0, time.UTC)
	timing := newTurnTiming(logger, "response-test", committedAt, 32000)
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

func TestDiscardLastResponseRemovesCanceledUserAssistantPair(t *testing.T) {
	session := &Session{history: []chatMessage{
		{Role: "user", Content: "first"},
		{Role: "assistant", Content: "answer"},
		{Role: "user", Content: "interrupted"},
		{Role: "assistant", Content: "partial"},
	}}
	if err := session.DiscardLastResponse(context.Background()); err != nil {
		t.Fatal(err)
	}
	if len(session.history) != 2 || session.history[0].Content != "first" ||
		session.history[1].Content != "answer" {
		t.Fatalf("history after discard = %#v", session.history)
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
