package app

import (
	"context"
	"crypto/tls"
	"encoding/json"
	"fmt"
	"io"
	"log/slog"
	"net/http"
	"sync"
	"testing"
	"time"

	"github.com/gorilla/websocket"
	"github.com/lst92991-eng/boomPI/server/internal/backend"
	"github.com/lst92991-eng/boomPI/server/internal/config"
	"github.com/lst92991-eng/boomPI/server/internal/protocol"
)

const testDeviceID = "00112233-4455-6677-8899-aabbccddeeff"

func TestDeviceStreamingRoundTripWithFakeProvider(t *testing.T) {
	t.Setenv("DASHSCOPE_API_KEY", "offline-test-key")
	t.Setenv("DASHSCOPE_WORKSPACE_ID", "offline-test-workspace")
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
		Version: protocol.Version, Type: "hello", MessageID: "client-1", DeviceID: testDeviceID, Payload: json.RawMessage(`{}`),
	})
	helloAck := readControl(t, webSocket)
	if helloAck.Type != "hello.ack" || helloAck.SessionID == 0 {
		t.Fatalf("hello.ack = %+v", helloAck)
	}

	turn := protocol.ControlEnvelope{
		Version: protocol.Version, MessageID: "client-2", DeviceID: testDeviceID,
		SessionID: helloAck.SessionID, TurnID: 11, StreamID: 12, Epoch: 1,
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
		Channels: 1, SampleRateHz: 16_000, PayloadLen: uint32(len(input)), Sequence: 0, Epoch: 1,
		DeviceUUID: deviceUUID, SessionID: helloAck.SessionID, TurnID: 11, StreamID: 12,
	}, input)
	turn.Type = "turn.commit"
	turn.MessageID = "client-3"
	turn.Payload = json.RawMessage(`{}`)
	writeControl(t, webSocket, turn)

	wantControls := []string{"response.start", "response.text_delta", "response.audio_start"}
	for _, want := range wantControls {
		if got := readControl(t, webSocket); got.Type != want {
			t.Fatalf("control type = %q, want %q", got.Type, want)
		}
	}
	messageType, frame, err := webSocket.ReadMessage()
	if err != nil {
		t.Fatalf("ReadMessage(PCM) error = %v", err)
	}
	if messageType != websocket.BinaryMessage {
		t.Fatalf("PCM WebSocket type = %d", messageType)
	}
	header, output, err := protocol.ParsePCMFrame(frame)
	if err != nil {
		t.Fatalf("ParsePCMFrame() error = %v", err)
	}
	if header.Kind != protocol.AudioKindDownlink || header.SampleRateHz != 24_000 || string(output) != string([]byte{1, 2, 3, 4}) {
		t.Fatalf("downlink header=%+v output=%v", header, output)
	}
	if header.TimestampUS > uint64(time.Minute/time.Microsecond) {
		t.Fatalf("downlink timestamp_us=%d looks like wall-clock time, want connection-monotonic time", header.TimestampUS)
	}
	if got := readControl(t, webSocket); got.Type != "response.done" {
		t.Fatalf("final control type = %q", got.Type)
	}

	provider.session.mu.Lock()
	recorded := append([]byte(nil), provider.session.audio...)
	provider.session.mu.Unlock()
	if len(recorded) != inputFrameBytes || recorded[0] != 7 || recorded[1] != 8 {
		t.Fatalf("provider input length=%d data=%v", len(recorded), recorded)
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

type roundTripBackend struct{ session *roundTripSession }

func newRoundTripBackend() *roundTripBackend {
	return &roundTripBackend{session: &roundTripSession{events: make(chan backend.ConversationEvent, 8)}}
}

func (b *roundTripBackend) Open(context.Context, backend.SessionConfig) (backend.ConversationSession, error) {
	return b.session, nil
}

type roundTripSession struct {
	mu        sync.Mutex
	audio     []byte
	events    chan backend.ConversationEvent
	closeOnce sync.Once
}

func (s *roundTripSession) SendAudio(_ context.Context, pcm []byte) error {
	s.mu.Lock()
	s.audio = append(s.audio, pcm...)
	s.mu.Unlock()
	return nil
}

func (s *roundTripSession) Commit(context.Context) error {
	for _, event := range []backend.ConversationEvent{
		{Type: backend.EventStarted, ResponseID: "response-1"},
		{Type: backend.EventTextDelta, ResponseID: "response-1", Text: "你好"},
		{Type: backend.EventAudio, ResponseID: "response-1", PCM: []byte{1, 2, 3, 4}, SampleRateHz: 24_000},
		{Type: backend.EventDone, ResponseID: "response-1"},
	} {
		s.events <- event
	}
	return nil
}

func (s *roundTripSession) Cancel(context.Context) error             { return nil }
func (s *roundTripSession) Events() <-chan backend.ConversationEvent { return s.events }
func (s *roundTripSession) Close() error {
	s.closeOnce.Do(func() { close(s.events) })
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
