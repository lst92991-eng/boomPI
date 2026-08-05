package app

import (
	"context"
	"encoding/json"
	"errors"
	"io"
	"log/slog"
	"sync"
	"sync/atomic"
	"testing"
	"time"

	"github.com/gorilla/websocket"
	"github.com/lst92991-eng/boomPI/server/internal/backend"
	"github.com/lst92991-eng/boomPI/server/internal/config"
	"github.com/lst92991-eng/boomPI/server/internal/protocol"
)

func TestProviderCancelFailureStillAcknowledgesLocalFence(t *testing.T) {
	t.Setenv("DASHSCOPE_API_KEY", "offline-test-key")
	t.Setenv("DASHSCOPE_WORKSPACE_ID", "offline-test-workspace")
	cfg, err := config.Load("")
	if err != nil {
		t.Fatalf("config.Load() error = %v", err)
	}
	cfg.ListenAddress = "127.0.0.1"
	cfg.WSSPort = freePort(t)
	cfg.DiscoveryPort = freeUDPPort(t)
	provider := newRoundTripBackend()
	provider.session.cancelErr = errors.New("test provider cancel failed")
	logger := slog.New(slog.NewTextHandler(io.Discard, nil))
	application, err := newWithBackend(cfg, logger, t.TempDir(), provider)
	if err != nil {
		t.Fatalf("newWithBackend() error = %v", err)
	}

	ctx, cancel := context.WithCancel(context.Background())
	serverDone := make(chan error, 1)
	go func() { serverDone <- application.Run(ctx) }()
	webSocket := dialTestServer(t, cfg.WSSPort)
	t.Cleanup(func() {
		_ = webSocket.Close()
		cancel()
	})

	writeControl(t, webSocket, protocol.ControlEnvelope{
		Version: protocol.Version, Type: "hello", MessageID: "client-hello", DeviceID: testDeviceID,
		Payload: json.RawMessage(`{"device_token":"` + testDeviceToken + `"}`),
	})
	helloAck := readControl(t, webSocket)
	turn := protocol.ControlEnvelope{
		Version: protocol.Version, Type: "turn.start", MessageID: "client-turn", DeviceID: testDeviceID,
		SessionID: helloAck.SessionID, TurnID: 51, StreamID: 52, Epoch: 2,
		Payload: json.RawMessage(`{"sample_rate_hz":16000}`),
	}
	writeControl(t, webSocket, turn)
	cancelTurn := turn
	cancelTurn.Type = "response.cancel"
	cancelTurn.MessageID = "client-cancel"
	cancelTurn.Payload = json.RawMessage(`{}`)
	writeControl(t, webSocket, cancelTurn)

	ack := readControl(t, webSocket)
	if ack.Type != "response.cancelled" || ack.SessionID != turn.SessionID ||
		ack.TurnID != turn.TurnID || ack.Epoch != turn.Epoch {
		t.Fatalf("response.cancelled after provider failure = %+v", ack)
	}
	select {
	case <-provider.session.closed:
	case <-time.After(time.Second):
		t.Fatal("failed provider session remained open after local cancel ACK")
	}
	cancel()
	select {
	case err := <-serverDone:
		if err != nil {
			t.Fatalf("App.Run() error = %v", err)
		}
	case <-time.After(2 * time.Second):
		t.Fatal("server did not stop")
	}
}

func TestProviderWarmupFailureKeepsPersistentSession(t *testing.T) {
	t.Setenv("DASHSCOPE_API_KEY", "offline-test-key")
	t.Setenv("DASHSCOPE_WORKSPACE_ID", "offline-test-workspace")
	cfg, err := config.Load("")
	if err != nil {
		t.Fatalf("config.Load() error = %v", err)
	}
	cfg.ListenAddress = "127.0.0.1"
	cfg.WSSPort = freePort(t)
	cfg.DiscoveryPort = freeUDPPort(t)
	provider := newRoundTripBackend()
	provider.session.failAudio.Store(true)
	logger := slog.New(slog.NewTextHandler(io.Discard, nil))
	application, err := newWithBackend(cfg, logger, t.TempDir(), provider)
	if err != nil {
		t.Fatalf("newWithBackend() error = %v", err)
	}

	ctx, cancel := context.WithCancel(context.Background())
	serverDone := make(chan error, 1)
	go func() { serverDone <- application.Run(ctx) }()
	webSocket := dialTestServer(t, cfg.WSSPort)
	t.Cleanup(func() {
		_ = webSocket.Close()
		cancel()
	})

	writeControl(t, webSocket, protocol.ControlEnvelope{
		Version: protocol.Version, Type: "hello", MessageID: "client-hello", DeviceID: testDeviceID,
		Payload: json.RawMessage(`{"device_token":"` + testDeviceToken + `"}`),
	})
	helloAck := readControl(t, webSocket)
	deviceUUID, err := protocol.ParseDeviceUUID(testDeviceID)
	if err != nil {
		t.Fatalf("ParseDeviceUUID() error = %v", err)
	}
	first := protocol.ControlEnvelope{
		Version: protocol.Version, Type: "turn.start", MessageID: "client-turn-1", DeviceID: testDeviceID,
		SessionID: helloAck.SessionID, TurnID: 31, StreamID: 32, Epoch: 2,
		Payload: json.RawMessage(`{"sample_rate_hz":16000}`),
	}
	writeControl(t, webSocket, first)
	input := make([]byte, inputFrameBytes)
	for sequence := uint32(0); sequence < 10; sequence++ {
		flags := uint16(0)
		if sequence == 0 {
			flags = protocol.PCMFlagStart
		}
		writeUplinkTestFrame(t, webSocket, deviceUUID, first, sequence, flags, input)
	}
	commit := first
	commit.Type = "turn.commit"
	commit.MessageID = "client-commit-1"
	commit.Payload = json.RawMessage(`{}`)
	writeControl(t, webSocket, commit)
	assertTurnError(t, readControl(t, webSocket), first)

	// The failed first provider call can leave PCM and commit already queued.
	// They belong to the retired identity and must not turn a local warm-up miss
	// into a fatal session protocol error.
	second := first
	second.MessageID = "client-turn-2"
	second.TurnID = 41
	second.StreamID = 42
	second.Epoch = 3
	writeControl(t, webSocket, second)
	writeUplinkTestFrame(t, webSocket, deviceUUID, second, 0,
		protocol.PCMFlagStart|protocol.PCMFlagEnd, input)
	second.Type = "turn.commit"
	second.MessageID = "client-commit-2"
	second.Payload = json.RawMessage(`{}`)
	writeControl(t, webSocket, second)
	if got := readControl(t, webSocket); got.Type != "response.start" || got.TurnID != second.TurnID {
		t.Fatalf("second turn response.start = %+v", got)
	}
	if got := provider.openCount.Load(); got != 1 {
		t.Fatalf("provider Open calls = %d, want one persistent session", got)
	}

	if err := webSocket.Close(); err != nil {
		t.Fatalf("WebSocket Close() error = %v", err)
	}
	select {
	case <-provider.session.closed:
	case <-time.After(time.Second):
		t.Fatal("provider session remained open after WebSocket close")
	}
	cancel()
	select {
	case err := <-serverDone:
		if err != nil {
			t.Fatalf("App.Run() error = %v", err)
		}
	case <-time.After(2 * time.Second):
		t.Fatal("server did not stop")
	}
}

func TestUplinkCongestionRetiresOnlyCurrentTurn(t *testing.T) {
	t.Setenv("DASHSCOPE_API_KEY", "offline-test-key")
	t.Setenv("DASHSCOPE_WORKSPACE_ID", "offline-test-workspace")
	cfg, err := config.Load("")
	if err != nil {
		t.Fatalf("config.Load() error = %v", err)
	}
	cfg.ListenAddress = "127.0.0.1"
	cfg.WSSPort = freePort(t)
	cfg.DiscoveryPort = freeUDPPort(t)
	provider := newBlockingUplinkBackend()
	logger := slog.New(slog.NewTextHandler(io.Discard, nil))
	application, err := newWithBackend(cfg, logger, t.TempDir(), provider)
	if err != nil {
		t.Fatalf("newWithBackend() error = %v", err)
	}

	ctx, cancel := context.WithCancel(context.Background())
	serverDone := make(chan error, 1)
	go func() { serverDone <- application.Run(ctx) }()
	webSocket := dialTestServer(t, cfg.WSSPort)
	t.Cleanup(func() {
		_ = webSocket.Close()
		cancel()
	})

	writeControl(t, webSocket, protocol.ControlEnvelope{
		Version: protocol.Version, Type: "hello", MessageID: "client-hello", DeviceID: testDeviceID,
		Payload: json.RawMessage(`{"device_token":"` + testDeviceToken + `"}`),
	})
	helloAck := readControl(t, webSocket)
	if helloAck.Type != "hello.ack" || helloAck.SessionID == 0 {
		t.Fatalf("hello.ack = %+v", helloAck)
	}
	deviceUUID, err := protocol.ParseDeviceUUID(testDeviceID)
	if err != nil {
		t.Fatalf("ParseDeviceUUID() error = %v", err)
	}

	first := protocol.ControlEnvelope{
		Version: protocol.Version, Type: "turn.start", MessageID: "client-turn-1", DeviceID: testDeviceID,
		SessionID: helloAck.SessionID, TurnID: 11, StreamID: 12, Epoch: 2,
		Payload: json.RawMessage(`{"sample_rate_hz":16000}`),
	}
	writeControl(t, webSocket, first)
	input := make([]byte, inputFrameBytes)
	writeUplinkTestFrame(t, webSocket, deviceUUID, first, 0, protocol.PCMFlagStart, input)
	select {
	case <-provider.session.firstAudioStarted:
	case <-time.After(time.Second):
		t.Fatal("provider did not block the first uplink frame")
	}
	// More than two receive queues of media arrive while the provider is
	// blocked. Transport must report one turn-local congestion event and keep
	// parsing heartbeats instead of cancelling the WebSocket.
	for sequence := uint32(1); sequence <= 70; sequence++ {
		writeUplinkTestFrame(t, webSocket, deviceUUID, first, sequence, 0, input)
	}
	commit := first
	commit.Type = "turn.commit"
	commit.MessageID = "client-commit-1"
	commit.Payload = json.RawMessage(`{}`)
	writeControl(t, webSocket, commit)
	time.Sleep(50 * time.Millisecond)
	close(provider.session.releaseFirstAudio)

	assertTurnError(t, readControl(t, webSocket), first)
	if got := provider.session.cancels.Load(); got != 1 {
		t.Fatalf("provider cancels after congestion = %d, want 1", got)
	}

	// Old PCM and the queued commit are discarded by turn identity. The same
	// authenticated socket and provider session must accept the next epoch.
	second := first
	second.MessageID = "client-turn-2"
	second.TurnID = 21
	second.StreamID = 22
	second.Epoch = 3
	writeControl(t, webSocket, second)
	writeUplinkTestFrame(t, webSocket, deviceUUID, second, 0,
		protocol.PCMFlagStart|protocol.PCMFlagEnd, input)
	second.Type = "turn.commit"
	second.MessageID = "client-commit-2"
	second.Payload = json.RawMessage(`{}`)
	writeControl(t, webSocket, second)
	if got := readControl(t, webSocket); got.Type != "response.start" || got.TurnID != second.TurnID {
		t.Fatalf("second turn response.start = %+v", got)
	}
	if got := readControl(t, webSocket); got.Type != "response.done" || got.TurnID != second.TurnID {
		t.Fatalf("second turn response.done = %+v", got)
	}
	if got := provider.openCount.Load(); got != 1 {
		t.Fatalf("provider Open calls = %d, want one persistent session", got)
	}

	if err := webSocket.Close(); err != nil {
		t.Fatalf("WebSocket Close() error = %v", err)
	}
	select {
	case <-provider.session.closed:
	case <-time.After(time.Second):
		t.Fatal("provider session remained open after WebSocket close")
	}
	cancel()
	select {
	case err := <-serverDone:
		if err != nil {
			t.Fatalf("App.Run() error = %v", err)
		}
	case <-time.After(2 * time.Second):
		t.Fatal("server did not stop")
	}
}

func writeUplinkTestFrame(
	t *testing.T,
	webSocket *websocket.Conn,
	deviceUUID [16]byte,
	turn protocol.ControlEnvelope,
	sequence uint32,
	flags uint16,
	pcm []byte,
) {
	t.Helper()
	writePCM(t, webSocket, protocol.PCMHeader{
		Version: protocol.Version, Kind: protocol.AudioKindUplink, Flags: flags,
		AudioFormat: protocol.AudioFormatPCM16LE, Channels: 1, SampleRateHz: 16_000,
		PayloadLen: uint32(len(pcm)), Sequence: sequence, DeviceUUID: deviceUUID,
		SessionID: turn.SessionID, TurnID: turn.TurnID, StreamID: turn.StreamID, Epoch: turn.Epoch,
	}, pcm)
}

type blockingUplinkBackend struct {
	session   *blockingUplinkSession
	openCount atomic.Int32
}

func newBlockingUplinkBackend() *blockingUplinkBackend {
	return &blockingUplinkBackend{session: &blockingUplinkSession{
		firstAudioStarted: make(chan struct{}), releaseFirstAudio: make(chan struct{}),
		events: make(chan backend.ConversationEvent, 2), closed: make(chan struct{}),
	}}
}

func (b *blockingUplinkBackend) Open(context.Context, backend.SessionConfig) (backend.ConversationSession, error) {
	b.openCount.Add(1)
	return b.session, nil
}

type blockingUplinkSession struct {
	firstAudioStarted chan struct{}
	releaseFirstAudio chan struct{}
	firstAudio        sync.Once
	events            chan backend.ConversationEvent
	closed            chan struct{}
	cancels           atomic.Int32
	closeOnce         sync.Once
}

func (s *blockingUplinkSession) SendAudio(ctx context.Context, _ []byte) error {
	var wait bool
	s.firstAudio.Do(func() {
		wait = true
		close(s.firstAudioStarted)
	})
	if !wait {
		return nil
	}
	select {
	case <-s.releaseFirstAudio:
		return nil
	case <-ctx.Done():
		return context.Cause(ctx)
	}
}

func (s *blockingUplinkSession) Commit(context.Context) error {
	s.events <- backend.ConversationEvent{Type: backend.EventStarted, ResponseID: "response-after-congestion"}
	s.events <- backend.ConversationEvent{Type: backend.EventDone, ResponseID: "response-after-congestion"}
	return nil
}

func (s *blockingUplinkSession) Cancel(context.Context) error {
	s.cancels.Add(1)
	return nil
}
func (s *blockingUplinkSession) Events() <-chan backend.ConversationEvent { return s.events }
func (s *blockingUplinkSession) Close() error {
	s.closeOnce.Do(func() {
		close(s.events)
		close(s.closed)
	})
	return nil
}
