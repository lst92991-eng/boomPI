package transport

import (
	"context"
	"crypto/tls"
	"encoding/json"
	"errors"
	"net/http"
	"net/http/httptest"
	"strings"
	"sync/atomic"
	"testing"
	"time"

	"github.com/gorilla/websocket"

	"github.com/lst92991-eng/boomPI/server/internal/protocol"
)

const (
	testTimeout      = 3 * time.Second
	testPingInterval = time.Second
	testPongTimeout  = 3 * time.Second
)

type receiveResult struct {
	message Message
	err     error
}

type receiveHandler struct {
	connected chan *Connection
	results   chan receiveResult
}

func newReceiveHandler() *receiveHandler {
	return &receiveHandler{
		connected: make(chan *Connection, 1),
		results:   make(chan receiveResult, 1),
	}
}

func (handler *receiveHandler) Handle(ctx context.Context, connection *Connection) error {
	handler.connected <- connection
	message, err := connection.Receive(ctx)
	handler.results <- receiveResult{message: message, err: err}
	return err
}

type blockingHandler struct {
	connected chan *Connection
	release   chan struct{}
}

func (handler *blockingHandler) Handle(ctx context.Context, connection *Connection) error {
	handler.connected <- connection
	select {
	case <-ctx.Done():
		return context.Cause(ctx)
	case <-handler.release:
		return nil
	}
}

type connectionLifetimeHandler struct {
	connected chan *Connection
	done      chan error
}

func (handler *connectionLifetimeHandler) Handle(ctx context.Context, connection *Connection) error {
	handler.connected <- connection
	<-ctx.Done()
	err := context.Cause(ctx)
	handler.done <- err
	return err
}

type sendHandler struct {
	envelope protocol.ControlEnvelope
	header   protocol.PCMHeader
	payload  []byte
	errors   chan error
}

func (handler *sendHandler) Handle(ctx context.Context, connection *Connection) error {
	if err := connection.SendControl(ctx, handler.envelope); err != nil {
		handler.errors <- err
		return err
	}
	if err := connection.SendPCM(ctx, handler.header, handler.payload); err != nil {
		handler.errors <- err
		return err
	}
	close(handler.errors)
	<-ctx.Done()
	return context.Cause(ctx)
}

func TestServerRequiresTLSConfiguration(t *testing.T) {
	handler := newReceiveHandler()
	if _, err := NewServer(Config{}, handler); err == nil || !strings.Contains(err.Error(), "TLS") {
		t.Fatalf("NewServer() error = %v, want a TLS configuration error", err)
	}
}

func TestServerRejectsPlaintextUpgrade(t *testing.T) {
	server := newTestTransportServer(t, newReceiveHandler())
	plainServer := httptest.NewServer(httpHandler(server))
	defer plainServer.Close()

	_, response, err := websocket.DefaultDialer.Dial(strings.Replace(plainServer.URL, "http://", "ws://", 1)+defaultPath, nil)
	if err == nil {
		t.Fatal("plaintext WebSocket upgrade unexpectedly succeeded")
	}
	if response == nil || response.StatusCode != 426 {
		t.Fatalf("plaintext status = %v, want 426", response)
	}
}

func TestConnectionReceivesTextControl(t *testing.T) {
	handler := newReceiveHandler()
	server, webSocket := startWSS(t, handler)
	defer stopWSS(server, webSocket)

	want := validControlEnvelope()
	data, err := protocol.EncodeControl(want)
	if err != nil {
		t.Fatalf("EncodeControl() error = %v", err)
	}
	if err := webSocket.WriteMessage(websocket.TextMessage, data); err != nil {
		t.Fatalf("WriteMessage(text) error = %v", err)
	}
	result := awaitResult(t, handler.results)
	if result.err != nil {
		t.Fatalf("Receive() error = %v", result.err)
	}
	if result.message.Control == nil || result.message.Control.Type != want.Type {
		t.Fatalf("received control = %#v", result.message.Control)
	}
	if result.message.PCMHeader != nil || result.message.PCM != nil {
		t.Fatalf("text event unexpectedly contains PCM: %#v", result.message)
	}
}

func TestConnectionReceivesBinaryPCM(t *testing.T) {
	handler := newReceiveHandler()
	server, webSocket := startWSS(t, handler)
	defer stopWSS(server, webSocket)

	header, payload := validPCMFrame(t)
	frame := marshalPCMFrame(t, header, payload)
	if err := webSocket.WriteMessage(websocket.BinaryMessage, frame); err != nil {
		t.Fatalf("WriteMessage(binary) error = %v", err)
	}
	result := awaitResult(t, handler.results)
	if result.err != nil {
		t.Fatalf("Receive() error = %v", result.err)
	}
	if result.message.PCMHeader == nil || *result.message.PCMHeader != header {
		t.Fatalf("received PCM header = %#v, want %#v", result.message.PCMHeader, header)
	}
	if string(result.message.PCM) != string(payload) || result.message.Control != nil {
		t.Fatalf("received PCM payload/control = %v/%#v", result.message.PCM, result.message.Control)
	}
}

func TestConnectionRejectsOversizedText(t *testing.T) {
	handler := newReceiveHandler()
	server, webSocket := startWSS(t, handler)
	defer stopWSS(server, webSocket)

	oversized := strings.Repeat("x", protocol.MaxControlMessageBytes+1)
	if err := webSocket.WriteMessage(websocket.TextMessage, []byte(oversized)); err != nil {
		t.Fatalf("WriteMessage(oversized) error = %v", err)
	}
	result := awaitResult(t, handler.results)
	if result.err == nil || !strings.Contains(result.err.Error(), "exceeds") {
		t.Fatalf("Receive() error = %v, want an explicit size error", result.err)
	}
}

func TestConnectionDistinguishesControlAndPCMErrors(t *testing.T) {
	tests := []struct {
		name        string
		messageType int
		data        []byte
		wantError   string
	}{
		{name: "control", messageType: websocket.TextMessage, data: []byte(`{"broken":true}`), wantError: "decode control frame"},
		{name: "pcm", messageType: websocket.BinaryMessage, data: []byte("not-a-pcm-frame"), wantError: "decode PCM frame"},
	}
	for _, test := range tests {
		t.Run(test.name, func(t *testing.T) {
			handler := newReceiveHandler()
			server, webSocket := startWSS(t, handler)
			defer stopWSS(server, webSocket)
			if err := webSocket.WriteMessage(test.messageType, test.data); err != nil {
				t.Fatalf("WriteMessage() error = %v", err)
			}
			result := awaitResult(t, handler.results)
			if result.err == nil || !strings.Contains(result.err.Error(), test.wantError) {
				t.Fatalf("Receive() error = %v, want %q", result.err, test.wantError)
			}
		})
	}
}

func TestConnectionCancellationUnblocksReceive(t *testing.T) {
	handler := newReceiveHandler()
	server, webSocket := startWSS(t, handler)
	defer stopWSS(server, webSocket)

	connection := awaitConnection(t, handler.connected)
	if err := connection.Close(); err != nil {
		t.Fatalf("Close() error = %v", err)
	}
	result := awaitResult(t, handler.results)
	if !errors.Is(result.err, errConnectionClosed) {
		t.Fatalf("Receive() error = %v, want connection closed", result.err)
	}
}

func TestReceiveHonorsCallerCancellation(t *testing.T) {
	handler := &blockingHandler{connected: make(chan *Connection, 1), release: make(chan struct{})}
	server, webSocket := startWSS(t, handler)
	defer stopWSS(server, webSocket)
	connection := awaitConnection(t, handler.connected)

	ctx, cancel := context.WithCancel(context.Background())
	cancel()
	if _, err := connection.Receive(ctx); !errors.Is(err, context.Canceled) {
		t.Fatalf("Receive(cancelled context) error = %v", err)
	}
	close(handler.release)
}

func TestServerAllowsOnlyOneActiveConnection(t *testing.T) {
	handler := &blockingHandler{connected: make(chan *Connection, 1), release: make(chan struct{})}
	transportServer := newTestTransportServer(t, handler)
	tlsServer := httptest.NewTLSServer(httpHandler(transportServer))
	defer tlsServer.Close()
	first := dialWSS(t, tlsServer, defaultPath)
	defer first.Close()
	_ = awaitConnection(t, handler.connected)

	dialer := websocket.Dialer{TLSClientConfig: &tls.Config{InsecureSkipVerify: true}}
	second, response, err := dialer.Dial(wssURL(tlsServer.URL, defaultPath), nil)
	if second != nil {
		_ = second.Close()
	}
	if err == nil {
		t.Fatal("second active connection unexpectedly succeeded")
	}
	if response == nil || response.StatusCode != 503 {
		t.Fatalf("second connection status = %v, want 503", response)
	}
	close(handler.release)
}

func TestConnectionSendsControlAndPCMThroughWriter(t *testing.T) {
	header, payload := validPCMFrame(t)
	handler := &sendHandler{
		envelope: validControlEnvelope(),
		header:   header,
		payload:  payload,
		errors:   make(chan error, 1),
	}
	server, webSocket := startWSS(t, handler)
	defer stopWSS(server, webSocket)

	messageType, data, err := webSocket.ReadMessage()
	if err != nil {
		t.Fatalf("ReadMessage(control) error = %v", err)
	}
	if messageType != websocket.TextMessage {
		t.Fatalf("control message type = %d", messageType)
	}
	if _, err := protocol.DecodeControl(data); err != nil {
		t.Fatalf("DecodeControl(outbound) error = %v", err)
	}
	messageType, data, err = webSocket.ReadMessage()
	if err != nil {
		t.Fatalf("ReadMessage(PCM) error = %v", err)
	}
	if messageType != websocket.BinaryMessage {
		t.Fatalf("PCM message type = %d", messageType)
	}
	if _, gotPayload, err := protocol.ParsePCMFrame(data); err != nil || string(gotPayload) != string(payload) {
		t.Fatalf("ParsePCMFrame(outbound) payload=%v error=%v", gotPayload, err)
	}
	for err := range handler.errors {
		t.Fatalf("outbound handler error = %v", err)
	}
}

func TestUnsolicitedPongsSuppressPingsAndThenTimeout(t *testing.T) {
	handler := &connectionLifetimeHandler{
		connected: make(chan *Connection, 1),
		done:      make(chan error, 1),
	}
	server, webSocket := startWSS(t, handler)
	defer stopWSS(server, webSocket)
	_ = awaitConnection(t, handler.connected)

	var pingCount atomic.Int32
	webSocket.SetPingHandler(func(string) error {
		pingCount.Add(1)
		return nil
	})
	readDone := make(chan struct{})
	go func() {
		defer close(readDone)
		for {
			if _, _, err := webSocket.ReadMessage(); err != nil {
				return
			}
		}
	}()

	pongTicker := time.NewTicker(200 * time.Millisecond)
	keepAliveTimer := time.NewTimer(testPongTimeout + 500*time.Millisecond)
	defer pongTicker.Stop()
	defer keepAliveTimer.Stop()
	writePong := func() {
		t.Helper()
		if err := webSocket.WriteControl(websocket.PongMessage, []byte("client-heartbeat"), time.Now().Add(time.Second)); err != nil {
			t.Fatalf("WriteControl(Pong) error = %v", err)
		}
	}
	writePong()
	for {
		select {
		case <-pongTicker.C:
			writePong()
		case <-keepAliveTimer.C:
			pongTicker.Stop()
			goto keptAlivePastTimeout
		case err := <-handler.done:
			t.Fatalf("connection ended while unsolicited Pongs were active: %v", err)
		}
	}

keptAlivePastTimeout:
	if got := pingCount.Load(); got != 0 {
		t.Fatalf("server Ping count while unsolicited Pongs were active = %d, want 0", got)
	}

	select {
	case err := <-handler.done:
		if err == nil || !strings.Contains(err.Error(), "timeout") {
			t.Fatalf("connection error after unsolicited Pongs stopped = %v, want timeout", err)
		}
	case <-time.After(testPongTimeout + time.Second):
		t.Fatal("connection did not time out after unsolicited Pongs stopped")
	}
	maxPingsAfterStop := int32((testPongTimeout + testPingInterval - 1) / testPingInterval)
	if got := pingCount.Load(); got == 0 || got > maxPingsAfterStop {
		t.Fatalf("server Ping count after unsolicited Pongs stopped = %d, want 1..%d", got, maxPingsAfterStop)
	}
	select {
	case <-readDone:
	case <-time.After(testTimeout):
		t.Fatal("client reader did not stop after connection timeout")
	}
}

func newTestTransportServer(t *testing.T, handler Handler) *Server {
	t.Helper()
	server, err := NewServer(Config{
		TLSConfig: &tls.Config{
			MinVersion: tls.VersionTLS12,
			GetCertificate: func(*tls.ClientHelloInfo) (*tls.Certificate, error) {
				return nil, errors.New("httptest owns the test certificate")
			},
		},
		PingInterval: testPingInterval,
		PongTimeout:  testPongTimeout,
	}, handler)
	if err != nil {
		t.Fatalf("NewServer() error = %v", err)
	}
	return server
}

func startWSS(t *testing.T, handler Handler) (*httptest.Server, *websocket.Conn) {
	t.Helper()
	transportServer := newTestTransportServer(t, handler)
	tlsServer := httptest.NewTLSServer(httpHandler(transportServer))
	webSocket := dialWSS(t, tlsServer, defaultPath)
	return tlsServer, webSocket
}

func httpHandler(server *Server) http.Handler {
	return http.HandlerFunc(server.serveHTTP)
}

func dialWSS(t *testing.T, server *httptest.Server, path string) *websocket.Conn {
	t.Helper()
	dialer := websocket.Dialer{TLSClientConfig: &tls.Config{InsecureSkipVerify: true}}
	connection, response, err := dialer.Dial(wssURL(server.URL, path), nil)
	if err != nil {
		t.Fatalf("Dial() response=%v error=%v", response, err)
	}
	return connection
}

func wssURL(serverURL, path string) string {
	return strings.Replace(serverURL, "https://", "wss://", 1) + path
}

func stopWSS(server *httptest.Server, connection *websocket.Conn) {
	_ = connection.Close()
	server.Close()
}

func awaitConnection(t *testing.T, connections <-chan *Connection) *Connection {
	t.Helper()
	select {
	case connection := <-connections:
		return connection
	case <-time.After(testTimeout):
		t.Fatal("timed out waiting for connection")
		return nil
	}
}

func awaitResult(t *testing.T, results <-chan receiveResult) receiveResult {
	t.Helper()
	select {
	case result := <-results:
		return result
	case <-time.After(testTimeout):
		t.Fatal("timed out waiting for receive result")
		return receiveResult{}
	}
}

func validControlEnvelope() protocol.ControlEnvelope {
	return protocol.ControlEnvelope{
		Version:   protocol.Version,
		Type:      "turn.start",
		MessageID: "transport-test-1",
		DeviceID:  "00112233-4455-6677-8899-aabbccddeeff",
		SessionID: 1,
		TurnID:    2,
		StreamID:  3,
		Epoch:     4,
		Payload:   json.RawMessage(`{"sample_rate_hz":16000}`),
	}
}

func validPCMFrame(t *testing.T) (protocol.PCMHeader, []byte) {
	t.Helper()
	deviceUUID, err := protocol.ParseDeviceUUID("00112233-4455-6677-8899-aabbccddeeff")
	if err != nil {
		t.Fatalf("ParseDeviceUUID() error = %v", err)
	}
	payload := []byte{1, 0, 2, 0, 3, 0, 4, 0}
	return protocol.PCMHeader{
		Version:      protocol.Version,
		Kind:         protocol.AudioKindUplink,
		AudioFormat:  protocol.AudioFormatPCM16LE,
		Channels:     1,
		SampleRateHz: 16000,
		PayloadLen:   uint32(len(payload)),
		Sequence:     1,
		TimestampUS:  42,
		Epoch:        4,
		DeviceUUID:   deviceUUID,
		SessionID:    1,
		TurnID:       2,
		StreamID:     3,
	}, payload
}

func marshalPCMFrame(t *testing.T, header protocol.PCMHeader, payload []byte) []byte {
	t.Helper()
	encoded, err := header.MarshalBinary()
	if err != nil {
		t.Fatalf("MarshalBinary() error = %v", err)
	}
	return append(encoded, payload...)
}
