package app

import (
	"context"
	"encoding/binary"
	"fmt"
	"io"
	"log/slog"
	"math"
	"net"
	"os"
	"strconv"
	"sync"
	"sync/atomic"
	"testing"
	"time"

	"github.com/lst92991-eng/boomPI/server/internal/backend"
	"github.com/lst92991-eng/boomPI/server/internal/config"
)

const (
	voiceLoopHILPrefix     = "BOOMPI_EXTERNAL_VOICE_LOOP_HIL"
	voiceLoopHILSampleRate = 24_000
	voiceLoopHILTonePeak   = 1_500
)

func TestExternalVoiceLoopHILServer(t *testing.T) {
	if os.Getenv(voiceLoopHILPrefix) != "1" {
		t.Skip("set BOOMPI_EXTERNAL_VOICE_LOOP_HIL=1 to run the offline external voice-loop HIL server")
	}
	targetTurns := voiceLoopHILEnvInt(t, voiceLoopHILPrefix+"_TURNS", 2, 1, 20)
	timeoutSeconds := voiceLoopHILEnvInt(t, voiceLoopHILPrefix+"_TIMEOUT_SECONDS", 120, 5, 600)
	port := voiceLoopHILEnvInt(t, voiceLoopHILPrefix+"_PORT", 0, 0, 65_535)

	// Config validation requires provider-shaped credentials. The injected
	// in-memory backend below never constructs or contacts Qwen.
	t.Setenv("DASHSCOPE_API_KEY", "offline-test-key")
	t.Setenv("DASHSCOPE_WORKSPACE_ID", "offline-test-workspace")
	cfg, err := config.Load("")
	if err != nil {
		t.Fatalf("config.Load() error = %v", err)
	}
	cfg.ListenAddress = "0.0.0.0"
	if port == 0 {
		port = freePort(t)
	}
	cfg.WSSPort = port
	cfg.DiscoveryPort = freeUDPPort(t)

	provider := &voiceLoopHILBackend{session: &voiceLoopHILSession{
		events: make(chan backend.ConversationEvent, 64),
		tone:   voiceLoopHILTonePCM(),
	}}
	logger := slog.New(slog.NewTextHandler(io.Discard, nil))
	identityDirectory := t.TempDir()
	writeFutureServerIdentity(t, identityDirectory)
	application, err := newWithBackend(cfg, logger, identityDirectory, provider)
	if err != nil {
		t.Fatalf("newWithBackend() error = %v", err)
	}

	serverCtx, stopServer := context.WithCancel(context.Background())
	serverDone := make(chan error, 1)
	go func() { serverDone <- application.Run(serverCtx) }()
	defer stopServer()

	if err := waitForTCPListener(net.JoinHostPort("127.0.0.1", strconv.Itoa(port)), 2*time.Second); err != nil {
		t.Fatalf("wait for external voice-loop WSS listener: %v", err)
	}
	fmt.Printf("BOOMPI_EXTERNAL_VOICE_LOOP_READY port=%d spki=%s target_turns=%d timeout_seconds=%d qwen=false\n",
		port, application.spkiPin, targetTurns, timeoutSeconds)

	deadline := time.Now().Add(time.Duration(timeoutSeconds) * time.Second)
	for int(provider.session.terminals()) < targetTurns {
		if time.Now().After(deadline) {
			t.Fatalf("voice-loop HIL timed out: commits=%d completed=%d cancelled=%d uplink_bytes=%d",
				provider.session.commits.Load(), provider.session.completed.Load(),
				provider.session.cancelled.Load(), provider.session.uplinkBytes.Load())
		}
		time.Sleep(10 * time.Millisecond)
	}
	// Allow the actor to forward the final provider terminal event before shutdown.
	time.Sleep(100 * time.Millisecond)
	stopServer()
	select {
	case runErr := <-serverDone:
		if runErr != nil {
			t.Fatalf("App.Run() error = %v", runErr)
		}
	case <-time.After(2 * time.Second):
		t.Fatal("external voice-loop server did not stop")
	}
	if provider.session.uplinkBytes.Load() == 0 {
		t.Fatal("voice-loop HIL received no uplink audio")
	}
	fmt.Printf("BOOMPI_EXTERNAL_VOICE_LOOP_PASS commits=%d completed=%d cancelled=%d uplink_bytes=%d qwen=false\n",
		provider.session.commits.Load(), provider.session.completed.Load(),
		provider.session.cancelled.Load(), provider.session.uplinkBytes.Load())
}

func voiceLoopHILEnvInt(t *testing.T, name string, fallback, minimum, maximum int) int {
	t.Helper()
	raw := os.Getenv(name)
	if raw == "" {
		return fallback
	}
	value, err := strconv.Atoi(raw)
	if err != nil || value < minimum || value > maximum {
		t.Fatalf("%s must be an integer in [%d,%d]", name, minimum, maximum)
	}
	return value
}

type voiceLoopHILBackend struct{ session *voiceLoopHILSession }

func (b *voiceLoopHILBackend) Open(context.Context, backend.SessionConfig) (backend.ConversationSession, error) {
	return b.session, nil
}

type voiceLoopHILSession struct {
	mu                            sync.Mutex
	events                        chan backend.ConversationEvent
	tone                          []byte
	responseStop                  context.CancelFunc
	responseDone                  chan struct{}
	pendingBytes                  int
	commits, completed, cancelled atomic.Int32
	uplinkBytes                   atomic.Int64
	closeOnce                     sync.Once
}

func (s *voiceLoopHILSession) SendAudio(ctx context.Context, pcm []byte) error {
	if err := ctx.Err(); err != nil {
		return err
	}
	if len(pcm) == 0 {
		return fmt.Errorf("voice-loop HIL received empty uplink audio")
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	s.pendingBytes += len(pcm)
	s.uplinkBytes.Add(int64(len(pcm)))
	return nil
}

func (s *voiceLoopHILSession) Commit(ctx context.Context) error {
	if err := ctx.Err(); err != nil {
		return err
	}
	s.mu.Lock()
	if s.pendingBytes == 0 || s.responseDone != nil {
		s.mu.Unlock()
		return fmt.Errorf("voice-loop HIL commit has no audio or another response is active")
	}
	s.pendingBytes = 0
	responseCtx, cancel := context.WithCancel(context.Background())
	done := make(chan struct{})
	s.responseStop, s.responseDone = cancel, done
	turn := s.commits.Add(1)
	s.mu.Unlock()
	go s.streamResponse(responseCtx, done, turn)
	return nil
}

func (s *voiceLoopHILSession) streamResponse(ctx context.Context, done chan struct{}, turn int32) {
	finished := false
	defer func() {
		if finished {
			s.completed.Add(1)
		} else {
			s.cancelled.Add(1)
		}
		s.mu.Lock()
		if s.responseDone == done {
			s.responseStop, s.responseDone = nil, nil
		}
		close(done)
		s.mu.Unlock()
	}()
	responseID := fmt.Sprintf("offline-voice-hil-%d", turn)
	if !s.emit(ctx, backend.ConversationEvent{Type: backend.EventStarted, ResponseID: responseID}) {
		return
	}
	ticker := time.NewTicker(20 * time.Millisecond)
	defer ticker.Stop()
	for offset := 0; offset < len(s.tone); offset += outputFrameBytes {
		end := offset + outputFrameBytes
		if end > len(s.tone) {
			end = len(s.tone)
		}
		if !s.emit(ctx, backend.ConversationEvent{Type: backend.EventAudio, ResponseID: responseID,
			PCM: s.tone[offset:end], SampleRateHz: voiceLoopHILSampleRate}) {
			return
		}
		select {
		case <-ctx.Done():
			return
		case <-ticker.C:
		}
	}
	finished = s.emit(ctx, backend.ConversationEvent{Type: backend.EventDone, ResponseID: responseID})
}

func (s *voiceLoopHILSession) emit(ctx context.Context, event backend.ConversationEvent) bool {
	select {
	case s.events <- event:
		return true
	case <-ctx.Done():
		return false
	}
}

func (s *voiceLoopHILSession) Cancel(ctx context.Context) error {
	s.mu.Lock()
	s.pendingBytes = 0
	stop, done := s.responseStop, s.responseDone
	if stop != nil {
		stop()
	}
	s.mu.Unlock()
	if done == nil {
		return nil
	}
	select {
	case <-done:
		return nil
	case <-ctx.Done():
		return ctx.Err()
	}
}

func (s *voiceLoopHILSession) Events() <-chan backend.ConversationEvent { return s.events }

func (s *voiceLoopHILSession) DiscardLastResponse(context.Context) error { return nil }

func (s *voiceLoopHILSession) Close() error {
	s.closeOnce.Do(func() {
		_ = s.Cancel(context.Background())
		close(s.events)
	})
	return nil
}

func (s *voiceLoopHILSession) terminals() int32 { return s.completed.Load() + s.cancelled.Load() }

func voiceLoopHILTonePCM() []byte {
	const sampleCount = voiceLoopHILSampleRate / 2
	const fadeSamples = voiceLoopHILSampleRate * 20 / 1_000
	pcm := make([]byte, sampleCount*2)
	for index := 0; index < sampleCount; index++ {
		phase := 2 * math.Pi * 440 * float64(index) / voiceLoopHILSampleRate
		envelope := 1.0
		if index < fadeSamples {
			envelope = float64(index) / fadeSamples
		} else if remaining := sampleCount - 1 - index; remaining < fadeSamples {
			envelope = float64(remaining) / fadeSamples
		}
		sample := int16(math.Round(voiceLoopHILTonePeak * envelope * math.Sin(phase)))
		binary.LittleEndian.PutUint16(pcm[index*2:], uint16(sample))
	}
	return pcm
}
