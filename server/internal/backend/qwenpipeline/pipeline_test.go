package qwenpipeline

import (
	"bytes"
	"encoding/json"
	"log/slog"
	"strings"
	"testing"
	"time"
)

func TestTurnTimingWritesOneSanitizedAggregateRecord(t *testing.T) {
	var output bytes.Buffer
	logger := slog.New(slog.NewJSONHandler(&output, nil))
	committedAt := time.Date(2026, time.July, 29, 12, 0, 0, 0, time.UTC)
	timing := newTurnTiming(logger, "response-test", committedAt, 32000)
	timing.markASRDone(committedAt.Add(100 * time.Millisecond))
	timing.markLLMFirstDelta(committedAt.Add(250 * time.Millisecond))
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
