package qwen

import (
	"context"
	"errors"
	"os"
	"strconv"
	"strings"
	"testing"
	"time"

	"github.com/lst92991-eng/boomPI/server/internal/backend"
)

const (
	liveInputSampleRateHz = 16_000
	livePCMBytesPerSample = 2
	livePCMFrameBytes     = liveInputSampleRateHz * livePCMBytesPerSample * 20 / 1000
	maxLiveInputBytes     = liveInputSampleRateHz * livePCMBytesPerSample * 10
)

func TestLiveOpenSession(t *testing.T) {
	if os.Getenv("BOOMPI_QWEN_LIVE_TEST") != "1" {
		t.Skip("set BOOMPI_QWEN_LIVE_TEST=1 for the explicit paid provider audio-turn smoke")
	}
	apiKey := strings.TrimSpace(os.Getenv("DASHSCOPE_API_KEY"))
	workspaceID := strings.TrimSpace(os.Getenv("DASHSCOPE_WORKSPACE_ID"))
	if apiKey == "" {
		t.Fatal("DASHSCOPE_API_KEY is required")
	}
	model := strings.TrimSpace(os.Getenv("BOOMPI_QWEN_LIVE_MODEL"))
	if model == "" {
		model = "qwen3.5-omni-plus-realtime"
	}
	voice := strings.TrimSpace(os.Getenv("BOOMPI_QWEN_LIVE_VOICE"))
	if voice == "" {
		voice = "Ethan"
	}
	testTimeout := liveTestTimeout(t)
	pcm := liveTestPCM(t)

	provider, err := New(Config{
		APIKey: apiKey, WorkspaceID: workspaceID, Region: RegionSingapore,
		Model: model, Voice: voice,
		Timeout: testTimeout, QueueSize: 64,
	})
	if err != nil {
		t.Fatalf("New() error = %v", redactLiveSecret(err, apiKey))
	}
	ctx, cancel := context.WithTimeout(context.Background(), testTimeout)
	defer cancel()
	session, err := provider.Open(ctx, backend.SessionConfig{
		DeviceID:     "00000000-0000-4000-8000-000000000001",
		SystemPrompt: "Reply briefly in Simplified Chinese. If the audio contains no intelligible speech, briefly say so.",
		Persona:      "Natural and concise.",
	})
	if err != nil {
		t.Fatalf("Open() error = %v", redactLiveSecret(err, apiKey))
	}
	defer func() {
		if closeErr := session.Close(); closeErr != nil {
			t.Errorf("Close() error = %v", redactLiveSecret(closeErr, apiKey))
		}
	}()

	for offset := 0; offset < len(pcm); offset += livePCMFrameBytes {
		end := offset + livePCMFrameBytes
		if end > len(pcm) {
			end = len(pcm)
		}
		if err := session.SendAudio(ctx, pcm[offset:end]); err != nil {
			t.Fatalf("SendAudio() error = %v", redactLiveSecret(err, apiKey))
		}
	}
	if err := session.Commit(ctx); err != nil {
		t.Fatalf("Commit() error = %v", redactLiveSecret(err, apiKey))
	}

	waitForLiveResponse(t, ctx, session.Events(), apiKey)
}

func liveTestPCM(t *testing.T) []byte {
	t.Helper()
	if path := strings.TrimSpace(os.Getenv("BOOMPI_QWEN_LIVE_PCM_FILE")); path != "" {
		pcm, err := os.ReadFile(path)
		if err != nil {
			t.Fatalf("read BOOMPI_QWEN_LIVE_PCM_FILE: %v", err)
		}
		validateLivePCM(t, pcm)
		return pcm
	}

	durationMS := 1_000
	if value := strings.TrimSpace(os.Getenv("BOOMPI_QWEN_LIVE_SILENCE_MS")); value != "" {
		parsed, err := strconv.Atoi(value)
		if err != nil || parsed < 200 || parsed > 5_000 {
			t.Fatal("BOOMPI_QWEN_LIVE_SILENCE_MS must be an integer from 200 to 5000")
		}
		durationMS = parsed
	}
	return make([]byte, liveInputSampleRateHz*livePCMBytesPerSample*durationMS/1000)
}

func validateLivePCM(t *testing.T, pcm []byte) {
	t.Helper()
	if len(pcm) == 0 || len(pcm)%livePCMBytesPerSample != 0 {
		t.Fatal("BOOMPI_QWEN_LIVE_PCM_FILE must contain non-empty raw 16 kHz mono S16_LE PCM")
	}
	if len(pcm) > maxLiveInputBytes {
		t.Fatalf("BOOMPI_QWEN_LIVE_PCM_FILE has %d bytes; the live smoke limit is %d bytes (10 seconds)", len(pcm), maxLiveInputBytes)
	}
}

func liveTestTimeout(t *testing.T) time.Duration {
	t.Helper()
	const defaultTimeout = 60 * time.Second
	value := strings.TrimSpace(os.Getenv("BOOMPI_QWEN_LIVE_TIMEOUT"))
	if value == "" {
		return defaultTimeout
	}
	timeout, err := time.ParseDuration(value)
	if err != nil || timeout < 5*time.Second || timeout > 2*time.Minute {
		t.Fatal("BOOMPI_QWEN_LIVE_TIMEOUT must be a duration from 5s to 2m")
	}
	return timeout
}

func waitForLiveResponse(t *testing.T, ctx context.Context, events <-chan backend.ConversationEvent, apiKey string) {
	t.Helper()
	var sawStarted, sawText, sawAudio bool
	for {
		select {
		case <-ctx.Done():
			t.Fatalf("live response timed out: %v", ctx.Err())
		case event, ok := <-events:
			if !ok {
				t.Fatal("provider event stream closed before response.done")
			}
			switch event.Type {
			case backend.EventStarted:
				sawStarted = true
			case backend.EventTextDelta:
				if event.Text != "" {
					sawText = true
				}
			case backend.EventAudio:
				if len(event.PCM) == 0 || len(event.PCM)%livePCMBytesPerSample != 0 || event.SampleRateHz != 24_000 {
					t.Fatalf("invalid provider audio event: bytes=%d sample_rate_hz=%d", len(event.PCM), event.SampleRateHz)
				}
				sawAudio = true
			case backend.EventDone:
				if !sawStarted {
					t.Fatal("response.done arrived without response.started")
				}
				if !sawText && !sawAudio {
					t.Fatal("response.done arrived without text or audio output")
				}
				return
			case backend.EventError:
				if event.Err == nil {
					t.Fatal("provider returned an error event without an error")
				}
				t.Fatalf("provider error = %v", redactLiveSecret(event.Err, apiKey))
			default:
				t.Fatalf("unexpected provider event type %d", event.Type)
			}
		}
	}
}

func redactLiveSecret(err error, secret string) error {
	if err == nil || secret == "" {
		return err
	}
	return errors.New(strings.ReplaceAll(err.Error(), secret, "<redacted>"))
}
