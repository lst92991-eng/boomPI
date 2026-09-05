package app

import (
	"context"
	"crypto/tls"
	"fmt"
	"io"
	"log/slog"
	"net"
	"strconv"
	"sync"
	"sync/atomic"
	"testing"
	"time"

	"github.com/gorilla/websocket"
	"github.com/lst92991-eng/boomPI/server/internal/backend"
	"github.com/lst92991-eng/boomPI/server/internal/config"
	"github.com/lst92991-eng/boomPI/server/internal/protocol"
)

const testDeviceID = "00112233-4455-6677-8899-aabbccddeeff"
const testDeviceToken = "boompi-teaching-shared-token-v1-2026"
const inputFrameBytes = protocol.UplinkFrameBytes
const outputFrameBytes = protocol.DownlinkFrameBytes

type roundTripBackend struct {
	session   *roundTripSession
	openCount atomic.Int32
}

func newRoundTripBackend() *roundTripBackend {
	return &roundTripBackend{session: &roundTripSession{
		events: make(chan backend.ConversationEvent, 128), closed: make(chan struct{}),
		audioBytes: 4,
	}}
}

func (b *roundTripBackend) Open(context.Context, backend.SessionConfig) (backend.ConversationSession, error) {
	b.openCount.Add(1)
	return b.session, nil
}

type roundTripSession struct {
	mu                         sync.Mutex
	audio                      []byte
	events                     chan backend.ConversationEvent
	closed                     chan struct{}
	once                       sync.Once
	commits, cancels, discards atomic.Int32
	audioBytes                 int
	cancelHook                 func(context.Context) error
}

func (s *roundTripSession) SendAudio(ctx context.Context, pcm []byte) error {
	if err := ctx.Err(); err != nil {
		return err
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	s.audio = append(s.audio, pcm...)
	return nil
}
func (s *roundTripSession) Commit(ctx context.Context) error {
	id := fmt.Sprint(s.commits.Add(1))
	events := []backend.ConversationEvent{
		{Type: backend.EventStarted, ResponseID: id},
		{Type: backend.EventTextDelta, ResponseID: id, Text: "你好"},
	}
	for offset := 0; offset < s.audioBytes; offset += outputFrameBytes {
		pcm := make([]byte, min(s.audioBytes-offset, outputFrameBytes))
		if len(pcm) >= 4 {
			pcm[0], pcm[2] = 1, 2
		}
		events = append(events, backend.ConversationEvent{Type: backend.EventAudio, ResponseID: id, PCM: pcm, SampleRateHz: 24000})
	}
	events = append(events, backend.ConversationEvent{Type: backend.EventDone, ResponseID: id})
	for _, event := range events {
		select {
		case s.events <- event:
		case <-ctx.Done():
			return ctx.Err()
		}
	}
	return nil
}
func (s *roundTripSession) Cancel(ctx context.Context) error {
	s.cancels.Add(1)
	if s.cancelHook != nil {
		return s.cancelHook(ctx)
	}
	return nil
}
func (s *roundTripSession) DiscardLastResponse(context.Context) error { s.discards.Add(1); return nil }
func (s *roundTripSession) Events() <-chan backend.ConversationEvent  { return s.events }
func (s *roundTripSession) Close() error {
	s.once.Do(func() { close(s.closed); close(s.events) })
	return nil
}

func startDeviceTest(t *testing.T, provider *roundTripBackend) (*websocket.Conn, config.Config) {
	t.Helper()
	t.Setenv("DASHSCOPE_API_KEY", "offline-test-key")
	t.Setenv("DASHSCOPE_WORKSPACE_ID", "offline-test-workspace")
	cfg, err := config.Load("")
	if err != nil {
		t.Fatal(err)
	}
	cfg.ListenAddress = "127.0.0.1"
	cfg.WSSPort, cfg.DiscoveryPort = freePort(t), freeUDPPort(t)
	app, err := newWithBackend(cfg, slog.New(slog.NewTextHandler(io.Discard, nil)), t.TempDir(), provider)
	if err != nil {
		t.Fatal(err)
	}
	ctx, cancel := context.WithCancel(context.Background())
	done := make(chan error, 1)
	go func() { done <- app.Run(ctx) }()
	t.Cleanup(func() {
		cancel()
		select {
		case err := <-done:
			if err != nil {
				t.Error(err)
			}
		case <-time.After(3 * time.Second):
			t.Error("server shutdown timed out")
		}
	})
	address := net.JoinHostPort(cfg.ListenAddress, strconv.Itoa(cfg.WSSPort))
	if err = waitForTCPListener(address, 2*time.Second); err != nil {
		t.Fatal(err)
	}
	dialer := websocket.Dialer{TLSClientConfig: &tls.Config{InsecureSkipVerify: true}, HandshakeTimeout: time.Second}
	conn, _, err := dialer.Dial("wss://"+address+"/ws", nil)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { conn.Close() })
	return conn, cfg
}

func helloDevice(t *testing.T, c *websocket.Conn) {
	t.Helper()
	writeControl(t, c, protocol.Control{Type: "hello", DeviceID: testDeviceID, Token: testDeviceToken})
	kind, data, err := readWire(c)
	if err != nil {
		t.Fatal(err)
	}
	control, err := protocol.DecodeControl(data)
	if err != nil || kind != websocket.TextMessage || control.Type != "ready" {
		t.Fatalf("ready: %q %v", data, err)
	}
}
func writeControl(t *testing.T, c *websocket.Conn, control protocol.Control) {
	t.Helper()
	wire, err := protocol.EncodeControl(control)
	if err == nil {
		err = c.WriteMessage(websocket.TextMessage, wire)
	}
	if err != nil {
		t.Fatal(err)
	}
}
func writePCM(t *testing.T, c *websocket.Conn, generation, sequence uint32, flags uint16) {
	t.Helper()
	pcm := make([]byte, inputFrameBytes)
	pcm[0], pcm[1] = 7, 8
	wire, err := protocol.EncodePCM(protocol.PCMHeader{Generation: generation, Sequence: sequence, Flags: flags}, pcm, true)
	if err == nil {
		err = c.WriteMessage(websocket.BinaryMessage, wire)
	}
	if err != nil {
		t.Fatal(err)
	}
}
func readWire(c *websocket.Conn) (int, []byte, error) {
	_ = c.SetReadDeadline(time.Now().Add(3 * time.Second))
	return c.ReadMessage()
}
func readReply(t *testing.T, c *websocket.Conn, generation uint32) ([]protocol.PCMHeader, int) {
	t.Helper()
	var headers []protocol.PCMHeader
	bytes := 0
	for {
		kind, wire, err := readWire(c)
		if err != nil {
			t.Fatal(err)
		}
		if kind == websocket.BinaryMessage {
			header, pcm, err := protocol.ParsePCMFrame(wire, false)
			if err != nil {
				t.Fatal(err)
			}
			if header.Generation != generation {
				t.Fatalf("stale PCM generation %d", header.Generation)
			}
			if header.Sequence != uint32(len(headers)) {
				t.Fatal("downlink sequence gap")
			}
			headers = append(headers, header)
			bytes += len(pcm)
		} else {
			control, err := protocol.DecodeControl(wire)
			if err != nil {
				t.Fatal(err)
			}
			if control.Generation != generation {
				t.Fatalf("stale control %s", wire)
			}
			if control.Type == "error" {
				t.Fatalf("server error %s", wire)
			}
			if control.Type == "done" {
				if len(headers) > 0 && headers[len(headers)-1].Flags&protocol.PCMFlagEnd == 0 {
					t.Fatal("done before PCM END")
				}
				return headers, bytes
			}
		}
	}
}

func TestV2StreamingRoundTripAndTerminalPCM(t *testing.T) {
	for _, audioBytes := range []int{0, 2, 4, 960, 962, 960 * 6} {
		t.Run(fmt.Sprint(audioBytes), func(t *testing.T) {
			provider := newRoundTripBackend()
			provider.session.audioBytes = audioBytes
			c, _ := startDeviceTest(t, provider)
			helloDevice(t, c)
			writePCM(t, c, 1, 0, protocol.PCMFlagStart|protocol.PCMFlagEnd)
			headers, count := readReply(t, c, 1)
			if count != audioBytes {
				t.Fatalf("got %d PCM bytes, want %d", count, audioBytes)
			}
			if len(headers) > 0 && headers[0].Flags&protocol.PCMFlagStart == 0 {
				t.Fatal("missing START")
			}
			if provider.session.commits.Load() != 1 {
				t.Fatal("END did not commit exactly once")
			}
		})
	}
}

func TestV2HistoryIntentAndStopTombstone(t *testing.T) {
	provider := newRoundTripBackend()
	c, _ := startDeviceTest(t, provider)
	helloDevice(t, c)
	writePCM(t, c, 1, 0, 3)
	readReply(t, c, 1)
	writePCM(t, c, 2, 0, 3)
	readReply(t, c, 2)
	if provider.session.discards.Load() != 0 {
		t.Fatal("NORMAL retracted history")
	}
	writePCM(t, c, 3, 0, 7)
	readReply(t, c, 3)
	if provider.session.discards.Load() != 1 {
		t.Fatal("SUPERSEDE must retract even after provider done")
	}
	retract := true
	writeControl(t, c, protocol.Control{Type: "stop", Generation: 5, Retract: &retract})
	// Frames from the old generation cannot resurrect after the STOP fence.
	writePCM(t, c, 3, 1, 2)
	writePCM(t, c, 9, 0, 3)
	readReply(t, c, 9)
	if provider.session.discards.Load() != 2 {
		t.Fatal("STOP retract=true was lost")
	}
	retract = false
	writeControl(t, c, protocol.Control{Type: "stop", Generation: 10, Retract: &retract})
	writePCM(t, c, 11, 0, 3)
	readReply(t, c, 11)
	if provider.session.discards.Load() != 2 {
		t.Fatal("STOP retract=false removed history")
	}
}

func TestV2RejectsGapsDuplicateStartAndPCMWithoutStart(t *testing.T) {
	for _, scenario := range []string{"gap", "duplicate_start", "missing_start", "after_end"} {
		t.Run(scenario, func(t *testing.T) {
			provider := newRoundTripBackend()
			c, _ := startDeviceTest(t, provider)
			helloDevice(t, c)
			if scenario == "missing_start" {
				writePCM(t, c, 1, 1, 2)
			} else {
				flags := uint16(1)
				if scenario == "after_end" {
					flags = 3
				}
				writePCM(t, c, 1, 0, flags)
				switch scenario {
				case "gap":
					writePCM(t, c, 1, 2, 2)
				case "duplicate_start":
					writePCM(t, c, 1, 0, 1)
				case "after_end":
					writePCM(t, c, 1, 1, 2)
				}
			}
			for i := 0; i < 20; i++ {
				_, _, err := readWire(c)
				if err != nil {
					return
				}
			}
			t.Fatal("invalid uplink remained open")
		})
	}
}

func TestV2AuthenticationRunsBeforeProviderOpen(t *testing.T) {
	provider := newRoundTripBackend()
	c, _ := startDeviceTest(t, provider)
	writeControl(t, c, protocol.Control{Type: "hello", DeviceID: testDeviceID, Token: "wrong-token"})
	if _, _, err := readWire(c); err == nil {
		t.Fatal("unauthenticated connection accepted")
	}
	if provider.openCount.Load() != 0 {
		t.Fatal("provider opened before authentication")
	}
}

func TestV2BlockedCancelAcceptsNewInputWithoutAcknowledgement(t *testing.T) {
	provider := newRoundTripBackend()
	entered, release := make(chan struct{}), make(chan struct{})
	var once sync.Once
	provider.session.cancelHook = func(ctx context.Context) error {
		if provider.session.commits.Load() != 1 {
			return nil
		}
		once.Do(func() { close(entered) })
		select {
		case <-release:
			return nil
		case <-ctx.Done():
			return ctx.Err()
		}
	}
	c, _ := startDeviceTest(t, provider)
	helloDevice(t, c)
	writePCM(t, c, 1, 0, 3)
	readReply(t, c, 1)
	writePCM(t, c, 2, 0, 5)
	select {
	case <-entered:
	case <-time.After(time.Second):
		t.Fatal("cancel did not start")
	}
	// Send the complete new utterance while cancellation is still blocked.
	for seq := uint32(1); seq <= 24; seq++ {
		flags := uint16(0)
		if seq == 24 {
			flags = 2
		}
		writePCM(t, c, 2, seq, flags)
	}
	close(release)
	readReply(t, c, 2)
	provider.session.mu.Lock()
	count := len(provider.session.audio)
	provider.session.mu.Unlock()
	if count != 26*inputFrameBytes {
		t.Fatalf("new utterance lost: got %d bytes", count)
	}
}

func TestNextPacedFrameDeadlineBoundsCatchUp(t *testing.T) {
	now := time.Now()
	for _, late := range []time.Duration{0, 3 * time.Millisecond, 10 * time.Millisecond, 100 * time.Millisecond} {
		next := nextPacedFrameDeadline(now.Add(-late), now)
		if next.Sub(now) < minimumOutputFrameSpacing || next.Sub(now) > outputFrameDuration {
			t.Fatalf("bad pacing %v", next.Sub(now))
		}
	}
}
