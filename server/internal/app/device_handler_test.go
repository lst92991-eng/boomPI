package app

import (
	"bytes"
	"context"
	"crypto/tls"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"log/slog"
	"net/http"
	"sync"
	"sync/atomic"
	"testing"
	"time"

	"github.com/gorilla/websocket"
	"github.com/lst92991-eng/boomPI/server/internal/backend"
	"github.com/lst92991-eng/boomPI/server/internal/config"
	"github.com/lst92991-eng/boomPI/server/internal/identity"
	"github.com/lst92991-eng/boomPI/server/internal/protocol"
	"github.com/lst92991-eng/boomPI/server/internal/transport"
)

const (
	testDeviceID    = "00112233-4455-6677-8899-aabbccddeeff"
	testDeviceToken = "boompi-teaching-shared-token-v1-2026"
)

func TestDeviceStreamingRoundTripWithFakeProvider(t *testing.T) {
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
	logger := slog.New(slog.NewTextHandler(io.Discard, nil))
	application, err := newWithBackend(cfg, logger, t.TempDir(), provider)
	if err != nil {
		t.Fatalf("newWithBackend() error = %v", err)
	}

	ctx, cancel := context.WithCancel(context.Background())
	serverDone := make(chan error, 1)
	go func() { serverDone <- application.Run(ctx) }()
	webSocket := dialTestServer(t, cfg.WSSPort)
	defer webSocket.Close()

	writeControl(t, webSocket, protocol.ControlEnvelope{
		Version: protocol.Version, Type: "hello", MessageID: "client-1", DeviceID: testDeviceID,
		Payload: json.RawMessage(`{"device_token":"` + testDeviceToken + `"}`),
	})
	helloAck := readControl(t, webSocket)
	if helloAck.Type != "hello.ack" || helloAck.SessionID == 0 {
		t.Fatalf("hello.ack = %+v", helloAck)
	}

	const currentEpoch uint32 = 2
	turn := protocol.ControlEnvelope{
		Version: protocol.Version, MessageID: "client-2", DeviceID: testDeviceID,
		SessionID: helloAck.SessionID, TurnID: 11, StreamID: 12, Epoch: currentEpoch,
	}
	turn.Type = "turn.start"
	turn.Payload = json.RawMessage(`{"sample_rate_hz":16000}`)
	writeControl(t, webSocket, turn)

	deviceUUID, err := protocol.ParseDeviceUUID(testDeviceID)
	if err != nil {
		t.Fatalf("ParseDeviceUUID() error = %v", err)
	}
	input := make([]byte, inputFrameBytes)
	input[0], input[1] = 7, 8
	writePCM(t, webSocket, protocol.PCMHeader{
		Version: protocol.Version, Kind: protocol.AudioKindUplink, AudioFormat: protocol.AudioFormatPCM16LE,
		Flags:    protocol.PCMFlagStart | protocol.PCMFlagEnd,
		Channels: 1, SampleRateHz: 16_000, PayloadLen: uint32(len(input)), Sequence: 0, Epoch: currentEpoch,
		DeviceUUID: deviceUUID, SessionID: helloAck.SessionID, TurnID: 11, StreamID: 12,
	}, input)
	turn.Type = "turn.commit"
	turn.MessageID = "client-3"
	turn.Payload = json.RawMessage(`{}`)
	writeControl(t, webSocket, turn)

	var downlinkStream uint32
	wantControls := []string{"response.start", "response.text_delta", "response.audio_start"}
	for _, want := range wantControls {
		got := readControl(t, webSocket)
		if got.Type != want {
			t.Fatalf("control type = %q, want %q", got.Type, want)
		}
		if got.Type == "response.start" {
			downlinkStream = got.StreamID
		}
	}
	wantOutput := responsePCMFixture()
	var output []byte
	for index, wantFlags := range []uint16{protocol.PCMFlagStart, protocol.PCMFlagEnd} {
		messageType, frame, err := webSocket.ReadMessage()
		if err != nil {
			t.Fatalf("ReadMessage(PCM %d) error = %v", index, err)
		}
		if messageType != websocket.BinaryMessage {
			t.Fatalf("PCM %d WebSocket type = %d", index, messageType)
		}
		header, payload, err := protocol.ParsePCMFrame(frame)
		if err != nil {
			t.Fatalf("ParsePCMFrame(%d) error = %v", index, err)
		}
		if header.Kind != protocol.AudioKindDownlink || header.SampleRateHz != 24_000 ||
			header.Sequence != uint32(index) || header.Flags != wantFlags {
			t.Fatalf("downlink header %d = %+v", index, header)
		}
		if index == 0 && len(payload) != outputFrameBytes {
			t.Fatalf("first downlink payload length = %d, want %d", len(payload), outputFrameBytes)
		}
		if header.TimestampUS > uint64(time.Minute/time.Microsecond) {
			t.Fatalf("downlink timestamp_us=%d looks like wall-clock time, want connection-monotonic time", header.TimestampUS)
		}
		output = append(output, payload...)
	}
	if !bytes.Equal(output, wantOutput) {
		t.Fatalf("downlink output length=%d, want %d", len(output), len(wantOutput))
	}
	if got := readControl(t, webSocket); got.Type != "response.done" {
		t.Fatalf("final control type = %q", got.Type)
	}
	if downlinkStream == 0 {
		t.Fatal("response.start did not provide a downlink stream")
	}

	cancelEnvelope := protocol.ControlEnvelope{
		Version: protocol.Version, Type: "response.cancel", MessageID: "client-4", DeviceID: testDeviceID,
		SessionID: helloAck.SessionID, TurnID: turn.TurnID, StreamID: downlinkStream, Epoch: currentEpoch - 1,
		Payload: json.RawMessage(`{}`),
	}
	writeControl(t, webSocket, cancelEnvelope)
	cancelEnvelope.MessageID = "client-5"
	cancelEnvelope.Epoch = currentEpoch
	writeControl(t, webSocket, cancelEnvelope)
	ack := readControl(t, webSocket)
	if ack.Type != "response.cancelled" || ack.SessionID != helloAck.SessionID ||
		ack.TurnID != turn.TurnID || ack.Epoch != currentEpoch || ack.StreamID == 0 {
		t.Fatalf("response.cancelled = %+v", ack)
	}
	if got := provider.session.discards.Load(); got != 1 {
		t.Fatalf("completed response discards = %d, want 1", got)
	}
	if err := webSocket.SetReadDeadline(time.Now().Add(150 * time.Millisecond)); err != nil {
		t.Fatalf("SetReadDeadline() error = %v", err)
	}
	if _, _, err := webSocket.ReadMessage(); err == nil {
		t.Fatal("stale generation produced an extra cancellation acknowledgement")
	}

	provider.session.mu.Lock()
	recorded := append([]byte(nil), provider.session.audio...)
	provider.session.mu.Unlock()
	if len(recorded) != inputFrameBytes || recorded[0] != 7 || recorded[1] != 8 {
		t.Fatalf("provider input length=%d data=%v", len(recorded), recorded)
	}
	if got := provider.openCount.Load(); got != 1 {
		t.Fatalf("provider Open calls = %d, want 1", got)
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

func TestInvalidTurnSequencesDoNotClosePersistentSession(t *testing.T) {
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
	logger := slog.New(slog.NewTextHandler(io.Discard, nil))
	application, err := newWithBackend(cfg, logger, t.TempDir(), provider)
	if err != nil {
		t.Fatalf("newWithBackend() error = %v", err)
	}

	ctx, cancel := context.WithCancel(context.Background())
	serverDone := make(chan error, 1)
	go func() { serverDone <- application.Run(ctx) }()
	webSocket := dialTestServer(t, cfg.WSSPort)

	writeControl(t, webSocket, protocol.ControlEnvelope{
		Version: protocol.Version, Type: "hello", MessageID: "client-1", DeviceID: testDeviceID,
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

	startTurn := func(messageID string, turnID, streamID, epoch uint32) protocol.ControlEnvelope {
		envelope := protocol.ControlEnvelope{
			Version: protocol.Version, Type: "turn.start", MessageID: messageID, DeviceID: testDeviceID,
			SessionID: helloAck.SessionID, TurnID: turnID, StreamID: streamID, Epoch: epoch,
			Payload: json.RawMessage(`{"sample_rate_hz":16000}`),
		}
		writeControl(t, webSocket, envelope)
		return envelope
	}

	// Without a usable turn identity the only client-valid reply is a
	// session-scoped error at the epoch negotiated by hello.ack.
	zeroIdentity := protocol.ControlEnvelope{
		Version: protocol.Version, Type: "turn.start", MessageID: "client-zero", DeviceID: testDeviceID,
		SessionID: helloAck.SessionID, Payload: json.RawMessage(`{"sample_rate_hz":16000}`),
	}
	writeControl(t, webSocket, zeroIdentity)
	assertSessionError(t, readControl(t, webSocket), helloAck)

	// A parseable turn.start with invalid business payload is rejected without
	// closing the authenticated socket or touching the provider actor.
	rejected := protocol.ControlEnvelope{
		Version: protocol.Version, Type: "turn.start", MessageID: "client-rejected", DeviceID: testDeviceID,
		SessionID: helloAck.SessionID, TurnID: 7, StreamID: 8, Epoch: 2,
		Payload: json.RawMessage(`{"sample_rate_hz":8000}`),
	}
	writeControl(t, webSocket, rejected)
	assertTurnError(t, readControl(t, webSocket), rejected)

	// Turn 1 is syntactically valid PCM, but violates the START framing rule.
	first := startTurn("client-2", 11, 12, 2)
	input := make([]byte, inputFrameBytes)
	writePCM(t, webSocket, protocol.PCMHeader{
		Version: protocol.Version, Kind: protocol.AudioKindUplink, AudioFormat: protocol.AudioFormatPCM16LE,
		Flags: protocol.PCMFlagEnd, Channels: 1, SampleRateHz: 16_000,
		PayloadLen: uint32(len(input)), DeviceUUID: deviceUUID,
		SessionID: first.SessionID, TurnID: first.TurnID, StreamID: first.StreamID, Epoch: first.Epoch,
	}, input)
	assertTurnError(t, readControl(t, webSocket), first)

	// The actor has retired epoch 2. Reusing that epoch is a turn error, not a
	// reason to rebuild the authenticated WebSocket/provider session.
	stale := startTurn("client-stale", 13, 14, 2)
	assertTurnError(t, readControl(t, webSocket), stale)

	// Turn 2 is also parseable, but commit is illegal before any complete input.
	second := startTurn("client-3", 21, 22, 3)
	second.Type = "turn.commit"
	second.MessageID = "client-4"
	second.Payload = json.RawMessage(`{}`)
	writeControl(t, webSocket, second)
	assertTurnError(t, readControl(t, webSocket), second)

	// The same authenticated socket and provider session must accept turn 3.
	third := startTurn("client-5", 31, 32, 4)
	duplicate := startTurn("client-duplicate", 41, 42, 5)
	assertTurnError(t, readControl(t, webSocket), duplicate)
	// Rejecting the newer request must not consume its epoch or retire the
	// already active turn. The original turn remains writable and committable.
	writePCM(t, webSocket, protocol.PCMHeader{
		Version: protocol.Version, Kind: protocol.AudioKindUplink, AudioFormat: protocol.AudioFormatPCM16LE,
		Flags: protocol.PCMFlagStart | protocol.PCMFlagEnd, Channels: 1, SampleRateHz: 16_000,
		PayloadLen: uint32(len(input)), DeviceUUID: deviceUUID,
		SessionID: third.SessionID, TurnID: third.TurnID, StreamID: third.StreamID, Epoch: third.Epoch,
	}, input)
	third.Type = "turn.commit"
	third.MessageID = "client-6"
	third.Payload = json.RawMessage(`{}`)
	writeControl(t, webSocket, third)
	if got := readControl(t, webSocket); got.Type != "response.start" || got.TurnID != third.TurnID {
		t.Fatalf("third turn response.start = %+v", got)
	}
	if got := provider.openCount.Load(); got != 1 {
		t.Fatalf("provider Open calls = %d, want one persistent session", got)
	}
	if got := provider.session.commits.Load(); got != 1 {
		t.Fatalf("provider commits = %d, want only the valid turn", got)
	}

	// A real peer close, unlike a turn error, ends the provider session.
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

func TestBlockedDownlinkReleasesTurnStateAndDoesNotPoisonNextTurn(t *testing.T) {
	turnCtx, stopTurn := context.WithCancel(context.Background())
	turn := activeTurn{
		sessionID: 1, turnID: 11, epoch: 2, active: true,
		downlinkCtx: turnCtx, stopDownlink: stopTurn,
	}
	state := &connectionState{turn: turn, monotonicStart: time.Now()}
	pacer := &pacedDownlink{epoch: turn.epoch, turnID: turn.turnID}
	_, _ = pacer.pending.Write(make([]byte, outputFrameBytes*2))
	handler := &deviceHandler{}

	writeStarted := make(chan struct{})
	result := make(chan error, 1)
	go func() {
		_, _, err := handler.sendNextPacedFrameWith(
			context.Background(), state, pacer,
			func(ctx context.Context, reserved activeTurn, _ []byte, _ bool) error {
				if reserved.outputSequence != 0 {
					return fmt.Errorf("reserved sequence = %d, want 0", reserved.outputSequence)
				}
				close(writeStarted)
				<-ctx.Done()
				return context.Cause(ctx)
			},
		)
		result <- err
	}()
	select {
	case <-writeStarted:
	case <-time.After(time.Second):
		t.Fatal("downlink write did not reach bounded transport wait")
	}

	// This is the lock ordering used by response.cancel: it must complete while
	// the transport writer remains blocked, then its turn context releases that
	// writer without ending the device session.
	retired := make(chan struct{})
	go func() {
		state.mu.Lock()
		state.turn.active = false
		state.turn.stopDownlink()
		state.mu.Unlock()
		close(retired)
	}()
	select {
	case <-retired:
	case <-time.After(200 * time.Millisecond):
		t.Fatal("response.cancel could not acquire turn state during downlink backpressure")
	}
	select {
	case err := <-result:
		if err != nil {
			t.Fatalf("retired downlink returned a session-fatal error: %v", err)
		}
	case <-time.After(time.Second):
		t.Fatal("retired downlink did not observe its turn fence")
	}

	nextCtx, stopNext := context.WithCancel(context.Background())
	defer stopNext()
	next := activeTurn{
		sessionID: 1, turnID: 21, epoch: 3, active: true,
		downlinkCtx: nextCtx, stopDownlink: stopNext,
	}
	state.mu.Lock()
	state.turn = next
	state.mu.Unlock()
	pacer.reset()
	pacer.begin(next, "response-next", time.Now())
	pacer.providerDone = true
	_, _ = pacer.pending.Write(make([]byte, outputFrameBytes))
	var nextSequence uint32
	sent, final, err := handler.sendNextPacedFrameWith(
		context.Background(), state, pacer,
		func(_ context.Context, reserved activeTurn, _ []byte, _ bool) error {
			nextSequence = reserved.outputSequence
			return nil
		},
	)
	if err != nil || !sent || !final || nextSequence != 0 {
		t.Fatalf("next turn downlink = sent:%t final:%t sequence:%d error:%v",
			sent, final, nextSequence, err)
	}
	state.mu.Lock()
	gotSequence := state.turn.outputSequence
	state.mu.Unlock()
	if gotSequence != 1 {
		t.Fatalf("next turn reserved sequence = %d, want 1", gotSequence)
	}
}

func assertTurnError(t *testing.T, envelope protocol.ControlEnvelope, turn protocol.ControlEnvelope) {
	t.Helper()
	if envelope.Type != "error" || envelope.SessionID != turn.SessionID ||
		envelope.TurnID != turn.TurnID || envelope.Epoch != turn.Epoch {
		t.Fatalf("turn-scoped error = %+v, want turn=%d epoch=%d", envelope, turn.TurnID, turn.Epoch)
	}
	var payload struct {
		Code string `json:"code"`
	}
	if err := json.Unmarshal(envelope.Payload, &payload); err != nil || payload.Code != "turn_error" {
		t.Fatalf("turn error payload = %s, error=%v", envelope.Payload, err)
	}
}

func assertSessionError(t *testing.T, envelope protocol.ControlEnvelope, helloAck protocol.ControlEnvelope) {
	t.Helper()
	if envelope.Type != "error" || envelope.SessionID != helloAck.SessionID ||
		envelope.TurnID != 0 || envelope.StreamID != 0 || envelope.Epoch != helloAck.Epoch {
		t.Fatalf("session-scoped error = %+v, want session=%d epoch=%d",
			envelope, helloAck.SessionID, helloAck.Epoch)
	}
	var payload struct {
		Code string `json:"code"`
	}
	if err := json.Unmarshal(envelope.Payload, &payload); err != nil || payload.Code != "turn_error" {
		t.Fatalf("session error payload = %s, error=%v", envelope.Payload, err)
	}
}

func TestDeviceHelloRejectsInvalidTokenBeforeProviderOpen(t *testing.T) {
	testCases := []struct {
		name    string
		payload json.RawMessage
	}{
		{name: "wrong token", payload: json.RawMessage(`{"device_token":"fedcba9876543210fedcba9876543210"}`)},
		{name: "missing token", payload: json.RawMessage(`{}`)},
		{name: "empty token", payload: json.RawMessage(`{"device_token":""}`)},
		{name: "unknown field", payload: json.RawMessage(`{"device_token":"` + testDeviceToken + `","extra":true}`)},
		{name: "uppercase alias", payload: json.RawMessage(`{"DEVICE_TOKEN":"` + testDeviceToken + `"}`)},
		{name: "case alias beside exact field", payload: json.RawMessage(`{"device_token":"` + testDeviceToken + `","DEVICE_TOKEN":"` + testDeviceToken + `"}`)},
	}

	for _, testCase := range testCases {
		t.Run(testCase.name, func(t *testing.T) {
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
			logger := slog.New(slog.NewTextHandler(io.Discard, nil))
			application, err := newWithBackend(cfg, logger, t.TempDir(), provider)
			if err != nil {
				t.Fatalf("newWithBackend() error = %v", err)
			}

			ctx, cancel := context.WithCancel(context.Background())
			serverDone := make(chan error, 1)
			go func() { serverDone <- application.Run(ctx) }()
			webSocket := dialTestServer(t, cfg.WSSPort)
			writeControl(t, webSocket, protocol.ControlEnvelope{
				Version: protocol.Version, Type: "hello", MessageID: "client-1", DeviceID: testDeviceID,
				Payload: testCase.payload,
			})
			if _, _, err := webSocket.ReadMessage(); err == nil {
				t.Error("invalid hello remained connected, want connection rejection")
			}
			if got := provider.openCount.Load(); got != 0 {
				t.Errorf("provider Open calls = %d, want 0", got)
			}
			_ = webSocket.Close()
			cancel()
			select {
			case err := <-serverDone:
				if err != nil {
					t.Fatalf("App.Run() error = %v", err)
				}
			case <-time.After(2 * time.Second):
				t.Fatal("server did not stop")
			}
		})
	}
}

func TestDeviceHelloTimesOutBeforeProviderOpen(t *testing.T) {
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
	logger := slog.New(slog.NewTextHandler(io.Discard, nil))
	application, err := newWithBackend(cfg, logger, t.TempDir(), provider)
	if err != nil {
		t.Fatalf("newWithBackend() error = %v", err)
	}

	ctx, cancel := context.WithCancel(context.Background())
	serverDone := make(chan error, 1)
	go func() { serverDone <- application.Run(ctx) }()
	webSocket := dialTestServer(t, cfg.WSSPort)
	defer webSocket.Close()
	_ = webSocket.SetReadDeadline(time.Now().Add(helloAuthenticationTimeout + 2*time.Second))

	started := time.Now()
	if _, _, err := webSocket.ReadMessage(); err == nil {
		t.Fatal("connection without hello remained open")
	}
	elapsed := time.Since(started)
	if elapsed < helloAuthenticationTimeout-500*time.Millisecond || elapsed > helloAuthenticationTimeout+2*time.Second {
		t.Fatalf("unauthenticated connection closed after %s, want approximately %s", elapsed, helloAuthenticationTimeout)
	}
	if got := provider.openCount.Load(); got != 0 {
		t.Fatalf("provider Open calls = %d, want 0", got)
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

func TestDeviceSessionIdleTimeoutUsesBusinessActivityAndReleasesDevice(t *testing.T) {
	t.Setenv("DASHSCOPE_API_KEY", "offline-test-key")
	t.Setenv("DASHSCOPE_WORKSPACE_ID", "offline-test-workspace")
	cfg, err := config.Load("")
	if err != nil {
		t.Fatalf("config.Load() error = %v", err)
	}
	cfg.ListenAddress = "127.0.0.1"
	cfg.WSSPort = freePort(t)
	cfg.SessionIdleTimeout = 1500 * time.Millisecond

	serverIdentity, err := identity.LoadOrCreate(t.TempDir())
	if err != nil {
		t.Fatalf("identity.LoadOrCreate() error = %v", err)
	}
	provider := newRoundTripBackend()
	logger := slog.New(slog.NewTextHandler(io.Discard, nil))
	handler := &deviceHandler{cfg: cfg, logger: logger, provider: provider}
	wss, err := transport.NewServer(transport.Config{
		Address:      fmt.Sprintf("127.0.0.1:%d", cfg.WSSPort),
		TLSConfig:    serverIdentity.TLSConfig,
		PingInterval: time.Second,
		PongTimeout:  3 * time.Second,
	}, handler)
	if err != nil {
		t.Fatalf("transport.NewServer() error = %v", err)
	}

	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	serverDone := make(chan error, 1)
	go func() { serverDone <- wss.Serve(ctx) }()
	webSocket := dialTestServer(t, cfg.WSSPort)
	defer webSocket.Close()
	writeControl(t, webSocket, protocol.ControlEnvelope{
		Version: protocol.Version, Type: "hello", MessageID: "client-1", DeviceID: testDeviceID,
		Payload: json.RawMessage(`{"device_token":"` + testDeviceToken + `"}`),
	})
	helloAck := readControl(t, webSocket)
	if helloAck.Type != "hello.ack" {
		t.Fatalf("hello.ack = %+v", helloAck)
	}
	if err := webSocket.SetReadDeadline(time.Now().Add(5 * time.Second)); err != nil {
		t.Fatalf("SetReadDeadline() error = %v", err)
	}
	peerDone := make(chan error, 1)
	go func() {
		for {
			if _, _, err := webSocket.ReadMessage(); err != nil {
				peerDone <- err
				return
			}
		}
	}()

	time.Sleep(900 * time.Millisecond)
	writeControl(t, webSocket, protocol.ControlEnvelope{
		Version: protocol.Version, Type: "turn.start", MessageID: "client-2", DeviceID: testDeviceID,
		SessionID: helloAck.SessionID, TurnID: 11, StreamID: 12, Epoch: 2,
		Payload: json.RawMessage(`{"sample_rate_hz":16000}`),
	})
	select {
	case <-provider.session.closed:
		t.Fatal("session expired at the original deadline despite business activity")
	case <-time.After(800 * time.Millisecond):
	}
	select {
	case <-provider.session.closed:
	case <-time.After(1300 * time.Millisecond):
		t.Fatal("transport ping incorrectly kept the idle provider session open")
	}
	select {
	case <-peerDone:
	case <-time.After(time.Second):
		t.Fatal("idle session did not close its WebSocket")
	}

	second := dialTestServer(t, cfg.WSSPort)
	_ = second.Close()
	cancel()
	select {
	case err := <-serverDone:
		if err != nil {
			t.Fatalf("transport.Serve() error = %v", err)
		}
	case <-time.After(2 * time.Second):
		t.Fatal("transport server did not stop")
	}
}

func TestInvalidTurnStartDoesNotRefreshDeviceSessionIdleTimeout(t *testing.T) {
	t.Setenv("DASHSCOPE_API_KEY", "offline-test-key")
	t.Setenv("DASHSCOPE_WORKSPACE_ID", "offline-test-workspace")
	cfg, err := config.Load("")
	if err != nil {
		t.Fatalf("config.Load() error = %v", err)
	}
	cfg.ListenAddress = "127.0.0.1"
	cfg.WSSPort = freePort(t)
	cfg.SessionIdleTimeout = 900 * time.Millisecond

	serverIdentity, err := identity.LoadOrCreate(t.TempDir())
	if err != nil {
		t.Fatalf("identity.LoadOrCreate() error = %v", err)
	}
	provider := newRoundTripBackend()
	handler := &deviceHandler{
		cfg: cfg, logger: slog.New(slog.NewTextHandler(io.Discard, nil)), provider: provider,
	}
	wss, err := transport.NewServer(transport.Config{
		Address: fmt.Sprintf("127.0.0.1:%d", cfg.WSSPort), TLSConfig: serverIdentity.TLSConfig,
		PingInterval: time.Second, PongTimeout: 3 * time.Second,
	}, handler)
	if err != nil {
		t.Fatalf("transport.NewServer() error = %v", err)
	}

	ctx, cancel := context.WithCancel(context.Background())
	serverDone := make(chan error, 1)
	go func() { serverDone <- wss.Serve(ctx) }()
	webSocket := dialTestServer(t, cfg.WSSPort)
	defer webSocket.Close()
	writeControl(t, webSocket, protocol.ControlEnvelope{
		Version: protocol.Version, Type: "hello", MessageID: "client-1", DeviceID: testDeviceID,
		Payload: json.RawMessage(`{"device_token":"` + testDeviceToken + `"}`),
	})
	helloAck := readControl(t, webSocket)

	time.Sleep(500 * time.Millisecond)
	writeControl(t, webSocket, protocol.ControlEnvelope{
		Version: protocol.Version, Type: "turn.start", MessageID: "client-invalid", DeviceID: testDeviceID,
		SessionID: helloAck.SessionID, TurnID: 11, StreamID: 12, Epoch: 2,
		Payload: json.RawMessage(`{"sample_rate_hz":8000}`),
	})
	if rejected := readControl(t, webSocket); rejected.Type != "error" {
		t.Fatalf("invalid turn.start response = %q, want error", rejected.Type)
	}
	select {
	case <-provider.session.closed:
	case <-time.After(650 * time.Millisecond):
		t.Fatal("invalid turn.start incorrectly refreshed the device session deadline")
	}

	cancel()
	select {
	case err := <-serverDone:
		if err != nil {
			t.Fatalf("transport.Serve() error = %v", err)
		}
	case <-time.After(2 * time.Second):
		t.Fatal("transport server did not stop")
	}
}

func TestStaleRetiredTurnCommitRequiresExactEmptyPayload(t *testing.T) {
	state := &connectionState{turn: activeTurn{
		deviceID: testDeviceID, sessionID: 7, turnID: 11,
		uplinkStream: 12, epoch: 2, active: false,
	}}
	commit := protocol.ControlEnvelope{
		Type: "turn.commit", DeviceID: testDeviceID, SessionID: 7,
		TurnID: 11, StreamID: 12, Epoch: 2, Payload: json.RawMessage(`{}`),
	}
	if !staleRetiredTurnControl(state, commit) {
		t.Fatal("exact stale turn.commit was not treated as idempotent")
	}
	commit.Payload = json.RawMessage(`{"extra":true}`)
	if staleRetiredTurnControl(state, commit) {
		t.Fatal("stale turn.commit with extra payload bypassed strict validation")
	}
}

func TestReportableSessionError(t *testing.T) {
	abnormalClose := &websocket.CloseError{
		Code: websocket.CloseAbnormalClosure,
		Text: "private peer details",
	}
	workerFailureCtx, cancelWorkerFailure := context.WithCancelCause(context.Background())
	cancelWorkerFailure(abnormalClose)
	idleCtx, cancelIdle := context.WithCancelCause(context.Background())
	cancelIdle(errDeviceSessionIdleTimeout)
	cleanupCtx, cancelCleanup := context.WithCancelCause(context.Background())
	cancelCleanup(context.Canceled)

	testCases := []struct {
		name string
		ctx  context.Context
		err  error
		want error
	}{
		{name: "nil result", ctx: context.Background()},
		{name: "normal cancellation", ctx: cleanupCtx, err: context.Canceled},
		{name: "EOF", ctx: context.Background(), err: fmt.Errorf("receive: %w", io.EOF)},
		{
			name: "normal WebSocket close", ctx: context.Background(),
			err: &websocket.CloseError{Code: websocket.CloseNormalClosure, Text: "normal"},
		},
		{
			name: "WebSocket going away", ctx: context.Background(),
			err: &websocket.CloseError{Code: websocket.CloseGoingAway, Text: "restart"},
		},
		{
			name: "worker cause recovered from canceled receive", ctx: workerFailureCtx,
			err: fmt.Errorf("receive: %w", context.Canceled), want: abnormalClose,
		},
		{
			name: "actual close is not masked by cleanup cancellation", ctx: cleanupCtx,
			err: abnormalClose, want: abnormalClose,
		},
		{
			name: "session idle timeout", ctx: idleCtx,
			err: context.Canceled, want: errDeviceSessionIdleTimeout,
		},
		{
			name: "deadline", ctx: context.Background(), err: context.DeadlineExceeded,
			want: context.DeadlineExceeded,
		},
		{
			name: "generic failure", ctx: context.Background(), err: errors.New("private internal details"),
			want: errors.New("private internal details"),
		},
	}

	for _, testCase := range testCases {
		t.Run(testCase.name, func(t *testing.T) {
			got := reportableSessionError(testCase.ctx, testCase.err)
			if (got == nil) != (testCase.want == nil) ||
				(got != nil && got.Error() != testCase.want.Error()) {
				t.Fatalf("reportableSessionError() = %v, want %v", got, testCase.want)
			}
		})
	}
}

type roundTripBackend struct {
	session   *roundTripSession
	openCount atomic.Int32
}

func newRoundTripBackend() *roundTripBackend {
	return &roundTripBackend{session: &roundTripSession{
		events: make(chan backend.ConversationEvent, 8),
		closed: make(chan struct{}),
	}}
}

func (b *roundTripBackend) Open(context.Context, backend.SessionConfig) (backend.ConversationSession, error) {
	b.openCount.Add(1)
	return b.session, nil
}

type roundTripSession struct {
	mu        sync.Mutex
	audio     []byte
	events    chan backend.ConversationEvent
	closed    chan struct{}
	failAudio atomic.Bool
	cancelErr error
	commits   atomic.Int32
	discards  atomic.Int32
	closeOnce sync.Once
}

func (s *roundTripSession) SendAudio(_ context.Context, pcm []byte) error {
	if s.failAudio.CompareAndSwap(true, false) {
		return errors.New("test provider is not ready")
	}
	s.mu.Lock()
	s.audio = append(s.audio, pcm...)
	s.mu.Unlock()
	return nil
}

func (s *roundTripSession) Commit(context.Context) error {
	s.commits.Add(1)
	responsePCM := responsePCMFixture()
	for _, event := range []backend.ConversationEvent{
		{Type: backend.EventStarted, ResponseID: "response-1"},
		{Type: backend.EventTextDelta, ResponseID: "response-1", Text: "你好"},
		{Type: backend.EventAudio, ResponseID: "response-1", PCM: responsePCM[:700], SampleRateHz: 24_000},
		{Type: backend.EventAudio, ResponseID: "response-1", PCM: responsePCM[700:], SampleRateHz: 24_000},
		{Type: backend.EventDone, ResponseID: "response-1"},
	} {
		s.events <- event
	}
	return nil
}

func responsePCMFixture() []byte {
	pcm := make([]byte, 1442)
	for index := range pcm {
		pcm[index] = byte(index % 251)
	}
	return pcm
}

func (s *roundTripSession) Cancel(context.Context) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.cancelErr
}
func (s *roundTripSession) DiscardLastResponse(context.Context) error {
	s.discards.Add(1)
	return nil
}
func (s *roundTripSession) Events() <-chan backend.ConversationEvent { return s.events }
func (s *roundTripSession) Close() error {
	s.closeOnce.Do(func() {
		close(s.events)
		close(s.closed)
	})
	return nil
}

func dialTestServer(t *testing.T, port int) *websocket.Conn {
	t.Helper()
	dialer := websocket.Dialer{TLSClientConfig: &tls.Config{InsecureSkipVerify: true}}
	endpoint := "wss://127.0.0.1:" + fmt.Sprint(port) + "/ws"
	deadline := time.Now().Add(2 * time.Second)
	for {
		connection, response, err := dialer.Dial(endpoint, http.Header{})
		if response != nil && response.Body != nil {
			_ = response.Body.Close()
		}
		if err == nil {
			_ = connection.SetReadDeadline(time.Now().Add(2 * time.Second))
			return connection
		}
		if time.Now().After(deadline) {
			t.Fatalf("Dial(%s) error = %v", endpoint, err)
		}
		time.Sleep(10 * time.Millisecond)
	}
}

func writeControl(t *testing.T, connection *websocket.Conn, envelope protocol.ControlEnvelope) {
	t.Helper()
	data, err := protocol.EncodeControl(envelope)
	if err != nil {
		t.Fatalf("EncodeControl() error = %v", err)
	}
	if err := connection.WriteMessage(websocket.TextMessage, data); err != nil {
		t.Fatalf("WriteMessage(control) error = %v", err)
	}
}

func readControl(t *testing.T, connection *websocket.Conn) protocol.ControlEnvelope {
	t.Helper()
	messageType, data, err := connection.ReadMessage()
	if err != nil {
		t.Fatalf("ReadMessage(control) error = %v", err)
	}
	if messageType != websocket.TextMessage {
		t.Fatalf("control WebSocket type = %d", messageType)
	}
	envelope, err := protocol.DecodeControl(data)
	if err != nil {
		t.Fatalf("DecodeControl() error = %v", err)
	}
	return envelope
}

func writePCM(t *testing.T, connection *websocket.Conn, header protocol.PCMHeader, pcm []byte) {
	t.Helper()
	encoded, err := header.MarshalBinary()
	if err != nil {
		t.Fatalf("MarshalBinary() error = %v", err)
	}
	if err := connection.WriteMessage(websocket.BinaryMessage, append(encoded, pcm...)); err != nil {
		t.Fatalf("WriteMessage(PCM) error = %v", err)
	}
}
