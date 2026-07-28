package qwen

import (
	"context"
	"os"
	"testing"
	"time"

	"github.com/lst92991-eng/boomPI/server/internal/backend"
)

// TestLiveRealtime is opt-in because it connects to a paid provider. The PCM
// input must be 16 kHz, mono, signed 16-bit little-endian audio.
func TestLiveRealtime(t *testing.T) {
	if os.Getenv("BOOMPI_LIVE_QWEN") != "1" {
		t.Skip("set BOOMPI_LIVE_QWEN=1 to run the paid provider test")
	}
	pcmPath := os.Getenv("BOOMPI_LIVE_PCM")
	outputPath := os.Getenv("BOOMPI_LIVE_OUTPUT_PCM")
	if pcmPath == "" {
		t.Fatal("BOOMPI_LIVE_PCM is required")
	}
	pcm, err := os.ReadFile(pcmPath)
	if err != nil {
		t.Fatalf("read live PCM: %v", err)
	}
	if len(pcm) == 0 || len(pcm)%2 != 0 || len(pcm) > 320_000 {
		t.Fatalf("live PCM must contain no more than 10 seconds of non-empty aligned 16 kHz S16_LE mono audio; bytes=%d", len(pcm))
	}

	provider, err := New(Config{
		APIKey:      os.Getenv("DASHSCOPE_API_KEY"),
		WorkspaceID: os.Getenv("DASHSCOPE_WORKSPACE_ID"),
		Region:      RegionChinaBeijing,
		Model:       "qwen3.5-omni-plus-realtime",
		Voice:       "Ethan",
		Timeout:     30 * time.Second,
		QueueSize:   32,
	})
	if err != nil {
		t.Fatalf("configure live provider: %v", err)
	}
	ctx, cancel := context.WithTimeout(context.Background(), 45*time.Second)
	defer cancel()
	opened, err := provider.Open(ctx, backend.SessionConfig{
		SystemPrompt: "Reply in one short sentence in Simplified Chinese.",
		Persona:      "You are boomPI, a concise voice assistant.",
	})
	if err != nil {
		t.Fatalf("open live provider: %v", err)
	}
	defer opened.Close()

	const chunkBytes = 3_200
	for offset := 0; offset < len(pcm); offset += chunkBytes {
		end := min(offset+chunkBytes, len(pcm))
		if err := opened.SendAudio(ctx, pcm[offset:end]); err != nil {
			t.Fatalf("send live PCM at byte %d: %v", offset, err)
		}
	}
	if err := opened.Commit(ctx); err != nil {
		t.Fatalf("commit live PCM: %v", err)
	}

	const maxLiveOutputBytes = 24_000 * 2 * 60
	var textBytes, audioBytes int
	var outputPCM []byte
	for {
		select {
		case <-ctx.Done():
			t.Fatalf("live response timeout: text_bytes=%d audio_bytes=%d", textBytes, audioBytes)
		case event, ok := <-opened.Events():
			if !ok {
				t.Fatalf("live response closed early: text_bytes=%d audio_bytes=%d", textBytes, audioBytes)
			}
			switch event.Type {
			case backend.EventTextDelta:
				textBytes += len(event.Text)
			case backend.EventAudio:
				if event.SampleRateHz != 24_000 {
					t.Fatalf("live audio sample rate=%d", event.SampleRateHz)
				}
				audioBytes += len(event.PCM)
				if outputPath != "" {
					if len(event.PCM) > maxLiveOutputBytes-len(outputPCM) {
						t.Fatalf("live audio exceeds the 60-second capture limit: bytes=%d", audioBytes)
					}
					outputPCM = append(outputPCM, event.PCM...)
				}
			case backend.EventError:
				t.Fatalf("live provider error: %v", event.Err)
			case backend.EventDone:
				if textBytes == 0 && audioBytes == 0 {
					t.Fatal("live response completed without text or audio")
				}
				if outputPath != "" {
					if len(outputPCM) == 0 {
						t.Fatal("live response did not contain audio to save")
					}
					if err := os.WriteFile(outputPath, outputPCM, 0o600); err != nil {
						t.Fatalf("write live output PCM: %v", err)
					}
				}
				t.Logf("live Qwen response received: text_bytes=%d audio_bytes=%d", textBytes, audioBytes)
				return
			}
		}
	}
}
