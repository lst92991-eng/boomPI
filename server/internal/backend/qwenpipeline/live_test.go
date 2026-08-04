package qwenpipeline

import (
	"context"
	"os"
	"strings"
	"testing"
	"time"

	"github.com/lst92991-eng/boomPI/server/internal/backend"
)

func TestLiveIntelligencePipeline(t *testing.T) {
	if os.Getenv("BOOMPI_LIVE_QWEN") != "1" {
		t.Skip("set BOOMPI_LIVE_QWEN=1 to run the paid provider test")
	}
	config := Config{
		APIKey:          os.Getenv("DASHSCOPE_API_KEY"),
		WorkspaceID:     os.Getenv("DASHSCOPE_WORKSPACE_ID"),
		Region:          RegionChinaBeijing,
		ASRModel:        "qwen3-asr-flash",
		ReasoningModel:  "qwen3.7-max",
		ReasoningEffort: "medium",
		TTSModel:        "qwen3-tts-flash-realtime",
		TTSVoice:        "Cherry",
		SearchMode:      "auto",
		Timeout:         90 * time.Second,
		QueueSize:       64,
	}
	if err := config.validate(); err != nil {
		t.Fatal(err)
	}
	ctx, cancel := context.WithTimeout(context.Background(), 3*time.Minute)
	defer cancel()

	tts := newTTSClient(config)
	var questionPCM []byte
	question := "请解释为什么柯西收敛准则不需要预先知道极限，并给出关键证明思路。"
	questionStream := make(chan string, 1)
	questionStream <- question
	close(questionStream)
	if err := tts.synthesizeStream(ctx, questionStream, func(pcm []byte) error {
		questionPCM = append(questionPCM, pcm...)
		return nil
	}); err != nil {
		t.Fatalf("synthesize question: %v", err)
	}
	if len(questionPCM) == 0 {
		t.Fatal("question TTS returned no PCM")
	}

	http := newHTTPClients(config)
	transcript, err := http.transcribe(ctx, questionPCM)
	if err != nil {
		t.Fatalf("transcribe question: %v", err)
	}
	if !strings.Contains(transcript, "柯西") {
		t.Fatalf("unexpected transcript: %q", transcript)
	}
	provider, err := New(config)
	if err != nil {
		t.Fatal(err)
	}
	session, err := provider.Open(ctx, backend.SessionConfig{
		SystemPrompt: "使用简体中文准确回答复杂问题，说明关键推理，不要回避。",
	})
	if err != nil {
		t.Fatal(err)
	}
	defer session.Close()
	if err := session.SendAudio(ctx, questionPCM); err != nil {
		t.Fatal(err)
	}
	started := time.Now()
	if err := session.Commit(ctx); err != nil {
		t.Fatal(err)
	}

	var answer strings.Builder
	answerBytes := 0
	var firstText, firstAudio time.Duration
	for event := range session.Events() {
		switch event.Type {
		case backend.EventTextDelta:
			if firstText == 0 {
				firstText = time.Since(started)
			}
			answer.WriteString(event.Text)
		case backend.EventAudio:
			if firstAudio == 0 {
				firstAudio = time.Since(started)
			}
			answerBytes += len(event.PCM)
		case backend.EventError:
			t.Fatalf("pipeline event error: %v", event.Err)
		case backend.EventDone:
			goto completed
		}
	}
	t.Fatal("pipeline event stream closed before done")

completed:
	if len([]rune(answer.String())) < 40 {
		t.Fatalf("reasoning answer is unexpectedly short: %q", answer.String())
	}
	if answerBytes == 0 {
		t.Fatal("answer TTS returned no PCM")
	}
	t.Logf("pipeline passed: transcript=%q answer_runes=%d answer_pcm_bytes=%d first_text=%s first_audio=%s total=%s",
		transcript, len([]rune(answer.String())), answerBytes, firstText, firstAudio, time.Since(started))
}
