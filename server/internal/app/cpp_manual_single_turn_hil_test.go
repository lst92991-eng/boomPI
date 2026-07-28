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
	manualSingleTurnHILUplinkBytes = 3 * 16_000 * 2
	manualSingleTurnHILSampleRate  = 24_000
	manualSingleTurnHILSampleCount = manualSingleTurnHILSampleRate / 2
	manualSingleTurnHILTonePeak    = 1_500
)

func TestCppManualSingleTurnExternalHILServer(t *testing.T) {
	if os.Getenv("BOOMPI_EXTERNAL_MANUAL_SINGLE_TURN_HIL") != "1" {
		t.Skip("set BOOMPI_EXTERNAL_MANUAL_SINGLE_TURN_HIL=1 to serve one external board manual turn")
	}

	// Config.Load validates provider settings, but newWithBackend receives the
	// in-memory fake below and therefore never constructs or contacts Qwen.
	t.Setenv("DASHSCOPE_API_KEY", "offline-test-key")
	t.Setenv("DASHSCOPE_WORKSPACE_ID", "offline-test-workspace")
	t.Setenv("BOOMPI_DEVICE_TOKEN", testDeviceToken)
	cfg, err := config.Load("", nil)
	if err != nil {
		t.Fatalf("config.Load() error = %v", err)
	}
	cfg.ListenAddress = "0.0.0.0"
	cfg.WSSPort = freePort(t)

	provider := newManualSingleTurnHILBackend()
	if got := len(provider.session.tonePCM); got != manualSingleTurnHILSampleCount*2 {
		t.Fatalf("safe prompt PCM bytes = %d, want %d", got, manualSingleTurnHILSampleCount*2)
	}
	if peak := maxPCM16LEAmplitude(provider.session.tonePCM); peak > 1_600 {
		t.Fatalf("safe prompt peak = %d, exceeds 1600", peak)
	}

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
	serverStopped := false
	defer func() {
		if serverStopped {
			return
		}
		stopServer()
		select {
		case runErr := <-serverDone:
			if runErr != nil {
				t.Errorf("App.Run() error = %v", runErr)
			}
		case <-time.After(2 * time.Second):
			t.Error("external manual-turn server did not stop")
		}
	}()

	loopbackAddress := net.JoinHostPort("127.0.0.1", strconv.Itoa(cfg.WSSPort))
	if err := waitForTCPListener(loopbackAddress, 2*time.Second); err != nil {
		t.Fatalf("wait for external manual-turn WSS listener: %v", err)
	}
	fmt.Printf("BOOMPI_EXTERNAL_MANUAL_SINGLE_TURN_READY port=%d spki=%s\n",
		cfg.WSSPort, application.spkiPin)

	deadline := time.Now().Add(60 * time.Second)
	for {
		recordedBytes := provider.session.recordedBytes()
		commits := provider.session.commits.Load()
		if recordedBytes > manualSingleTurnHILUplinkBytes {
			t.Fatalf("external board sent %d bytes, want %d", recordedBytes, manualSingleTurnHILUplinkBytes)
		}
		if commits > 1 {
			t.Fatalf("external board committed %d turns, want 1", commits)
		}
		if recordedBytes == manualSingleTurnHILUplinkBytes && commits == 1 {
			break
		}
		select {
		case <-provider.session.closed:
			t.Fatalf("external board session closed early: uplink_bytes=%d commits=%d",
				recordedBytes, commits)
		default:
		}
		select {
		case runErr := <-serverDone:
			serverStopped = true
			t.Fatalf("external manual-turn server stopped early: %v", runErr)
		default:
		}
		if time.Now().After(deadline) {
			t.Fatalf("external board manual turn timed out: uplink_bytes=%d commits=%d",
				recordedBytes, commits)
		}
		time.Sleep(10 * time.Millisecond)
	}

	select {
	case <-provider.session.closed:
	case <-time.After(5 * time.Second):
		t.Fatal("external board did not close the committed manual-turn session")
	}
	if got := provider.session.recordedBytes(); got != manualSingleTurnHILUplinkBytes {
		t.Fatalf("final external-board uplink bytes = %d, want %d", got, manualSingleTurnHILUplinkBytes)
	}
	if got := provider.session.commits.Load(); got != 1 {
		t.Fatalf("final external-board commits = %d, want 1", got)
	}
	if got := provider.openCount.Load(); got != 1 {
		t.Fatalf("provider Open calls = %d, want 1", got)
	}

	stopServer()
	select {
	case runErr := <-serverDone:
		serverStopped = true
		if runErr != nil {
			t.Fatalf("App.Run() error = %v", runErr)
		}
	case <-time.After(2 * time.Second):
		t.Fatal("external manual-turn server did not stop")
	}
	fmt.Printf("BOOMPI_EXTERNAL_MANUAL_SINGLE_TURN_PASS uplink_bytes=%d commits=1\n",
		manualSingleTurnHILUplinkBytes)
}

type manualSingleTurnHILBackend struct {
	session   *manualSingleTurnHILSession
	openCount atomic.Int32
}

func newManualSingleTurnHILBackend() *manualSingleTurnHILBackend {
	return &manualSingleTurnHILBackend{session: &manualSingleTurnHILSession{
		events:  make(chan backend.ConversationEvent, 16),
		closed:  make(chan struct{}),
		tonePCM: safeManualSingleTurnHILTonePCM(),
	}}
}

func (b *manualSingleTurnHILBackend) Open(context.Context, backend.SessionConfig) (backend.ConversationSession, error) {
	b.openCount.Add(1)
	return b.session, nil
}

type manualSingleTurnHILSession struct {
	audioBytes atomic.Int64
	events     chan backend.ConversationEvent
	closed     chan struct{}
	tonePCM    []byte
	commits    atomic.Int32
	closeOnce  sync.Once
}

func (s *manualSingleTurnHILSession) SendAudio(_ context.Context, pcm []byte) error {
	s.audioBytes.Add(int64(len(pcm)))
	return nil
}

func (s *manualSingleTurnHILSession) Commit(context.Context) error {
	s.commits.Add(1)
	s.events <- backend.ConversationEvent{Type: backend.EventStarted, ResponseID: "manual-hil-response"}

	// These even-sized provider chunks intentionally do not align with the
	// server's 960-byte downlink frame size. Their sum is exactly 500 ms.
	chunkSizes := [...]int{318, 1_558, 742, 4_094, 3_002, 614, 2_318, 4_090, 7_264}
	offset := 0
	for _, size := range chunkSizes {
		s.events <- backend.ConversationEvent{
			Type: backend.EventAudio, ResponseID: "manual-hil-response",
			PCM: s.tonePCM[offset : offset+size], SampleRateHz: manualSingleTurnHILSampleRate,
		}
		offset += size
	}
	s.events <- backend.ConversationEvent{Type: backend.EventDone, ResponseID: "manual-hil-response"}
	return nil
}

func (s *manualSingleTurnHILSession) Cancel(context.Context) error { return nil }

func (s *manualSingleTurnHILSession) Events() <-chan backend.ConversationEvent { return s.events }

func (s *manualSingleTurnHILSession) Close() error {
	s.closeOnce.Do(func() {
		close(s.events)
		close(s.closed)
	})
	return nil
}

func (s *manualSingleTurnHILSession) recordedBytes() int {
	return int(s.audioBytes.Load())
}

func safeManualSingleTurnHILTonePCM() []byte {
	pcm := make([]byte, manualSingleTurnHILSampleCount*2)
	const fadeSamples = manualSingleTurnHILSampleRate * 20 / 1_000
	for sampleIndex := 0; sampleIndex < manualSingleTurnHILSampleCount; sampleIndex++ {
		phase := 2 * math.Pi * 440 * float64(sampleIndex) / manualSingleTurnHILSampleRate
		envelope := 1.0
		if sampleIndex < fadeSamples {
			envelope = float64(sampleIndex) / fadeSamples
		} else if remaining := manualSingleTurnHILSampleCount - 1 - sampleIndex; remaining < fadeSamples {
			envelope = float64(remaining) / fadeSamples
		}
		sample := int16(math.Round(manualSingleTurnHILTonePeak * envelope * math.Sin(phase)))
		binary.LittleEndian.PutUint16(pcm[sampleIndex*2:], uint16(sample))
	}
	return pcm
}

func maxPCM16LEAmplitude(pcm []byte) int {
	peak := 0
	for offset := 0; offset+1 < len(pcm); offset += 2 {
		sample := int(int16(binary.LittleEndian.Uint16(pcm[offset:])))
		if sample < 0 {
			sample = -sample
		}
		if sample > peak {
			peak = sample
		}
	}
	return peak
}
