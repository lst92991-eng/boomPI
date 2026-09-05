package transport

import (
	"context"
	"crypto/tls"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"

	"github.com/gorilla/websocket"
)

type handlerFunc func(context.Context, *Connection) error

func (f handlerFunc) Handle(ctx context.Context, c *Connection) error { return f(ctx, c) }

func TestStaleQueuedWriteCannotCrossGenerationFence(t *testing.T) {
	c := &Connection{}
	c.fence.Store(2)
	// This returns before touching the socket, proving queued old writes are
	// suppressed even when they were enqueued before the START was received.
	if err := c.write(outboundMessage{generation: 1}); err != nil {
		t.Fatal(err)
	}
}

func TestWebSocketTLSOriginAndSingleDevice(t *testing.T) {
	connected := make(chan *Connection, 1)
	handler := handlerFunc(func(ctx context.Context, c *Connection) error { connected <- c; <-ctx.Done(); return nil })
	host := httptest.NewTLSServer(nil)
	defer host.Close()
	server, err := NewServer(Config{TLSConfig: host.TLS}, handler)
	if err != nil {
		t.Fatal(err)
	}
	host.Config.Handler = http.HandlerFunc(server.serveHTTP)
	dialer := websocket.Dialer{TLSClientConfig: &tls.Config{InsecureSkipVerify: true}, HandshakeTimeout: time.Second}
	url := "wss" + strings.TrimPrefix(host.URL, "https") + "/ws"
	if socket, _, err := dialer.Dial(url, http.Header{"Origin": []string{"https://other.example"}}); err == nil {
		socket.Close()
		t.Fatal("browser origin accepted")
	}
	first, _, err := dialer.Dial(url, nil)
	if err != nil {
		t.Fatal(err)
	}
	defer first.Close()
	c := <-connected
	defer c.Close()
	if second, response, err := dialer.Dial(url, nil); err == nil {
		second.Close()
		t.Fatal("second device accepted")
	} else if response == nil || response.StatusCode != http.StatusServiceUnavailable {
		t.Fatalf("unexpected second-device response %v", response)
	}
}

func TestServerRejectsUnsafeTLSAndHeartbeatConfiguration(t *testing.T) {
	handler := handlerFunc(func(context.Context, *Connection) error { return nil })
	for _, cfg := range []Config{
		{}, {TLSConfig: &tls.Config{}},
		{TLSConfig: &tls.Config{Certificates: []tls.Certificate{{}}, MinVersion: tls.VersionTLS10}},
		{TLSConfig: &tls.Config{Certificates: []tls.Certificate{{}}}, PingInterval: time.Millisecond},
		{TLSConfig: &tls.Config{Certificates: []tls.Certificate{{}}}, PongTimeout: time.Second},
	} {
		if _, err := NewServer(cfg, handler); err == nil {
			t.Fatal("unsafe configuration accepted")
		}
	}
}
