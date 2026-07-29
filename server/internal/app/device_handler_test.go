package app

import (
	"bytes"
	"context"
	"crypto/tls"
	"encoding/json"
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
	"github.com/lst92991-eng/boomPI/server/internal/protocol"
)

const (
	testDeviceID    = "00112233-4455-6677-8899-aabbccddeeff"
	testDeviceToken = "0123456789abcdef0123456789abcdef"
)

func TestDeviceStreamingRoundTripWithFakeProvider(t *testing.T) {
	t.Setenv("DASHSCOPE_API_KEY", "offline-test-key")
	t.Setenv("DASHSCOPE_WORKSPACE_ID", "offline-test-workspace")
	t.Setenv("BOOMPI_DEVICE_TOKEN", testDeviceToken)
	cfg, err := config.Load("", nil)
	if err != nil {
		t.Fatalf("config.Load() error = %v", err)
	}
	cfg.ListenAddress = "127.0.0.1"
	cfg.WSSPort = freePort(t)
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

func TestDeviceHelloRejectsInvalidTokenBeforeProviderOpen(t *testing.T) {
	testCases := []struct {
		name    string
		payload json.RawMessage
	}{
		{name: "wrong token", payload: json.RawMessage(`{"device_token":"fedcba9876543210fedcba9876543210"}`)},
		{name: "missing token", payload: json.RawMessage(`{}`)},
		{name: "empty token", payload: json.RawMessage(`{"device_token":""}`)},
		{name: "unknown field", payload: json.RawMessage(`{"device_token":"` + testDeviceToken + `","extra":true}`)},
	}

	for _, testCase := range testCases {
		t.Run(testCase.name, func(t *testing.T) {
			t.Setenv("DASHSCOPE_API_KEY", "offline-test-key")
			t.Setenv("DASHSCOPE_WORKSPACE_ID", "offline-test-workspace")
			t.Setenv("BOOMPI_DEVICE_TOKEN", testDeviceToken)
			cfg, err := config.Load("", nil)
			if err != nil {
				t.Fatalf("config.Load() error = %v", err)
			}
			cfg.ListenAddress = "127.0.0.1"
			cfg.WSSPort = freePort(t)
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
	t.Setenv("BOOMPI_DEVICE_TOKEN", testDeviceToken)
	cfg, err := config.Load("", nil)
	if err != nil {
		t.Fatalf("config.Load() error = %v", err)
	}
	cfg.ListenAddress = "127.0.0.1"
	cfg.WSSPort = freePort(t)
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
	commits   atomic.Int32
	closeOnce sync.Once
}

func (s *roundTripSession) SendAudio(_ context.Context, pcm []byte) error {
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

func (s *roundTripSession) Cancel(context.Context) error             { return nil }
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
