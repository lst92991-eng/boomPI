package discovery

import (
	"context"
	"encoding/base64"
	"fmt"
	"net"
	"strings"
	"testing"
	"time"
)

func TestResponderAnswersFixedRequestAndStopsWithContext(t *testing.T) {
	spki := base64.StdEncoding.EncodeToString(make([]byte, 32))
	responder, err := NewResponder(Config{
		BindAddress: "127.0.0.1", UDPPort: 17807, WSSPort: 17806,
	}, spki)
	if err != nil {
		t.Fatalf("NewResponder() error = %v", err)
	}
	listener, err := net.ListenUDP("udp", &net.UDPAddr{IP: net.ParseIP("127.0.0.1")})
	if err != nil {
		t.Fatalf("ListenUDP() error = %v", err)
	}
	ctx, cancel := context.WithCancel(context.Background())
	done := make(chan error, 1)
	go func() { done <- responder.serveConnection(ctx, listener) }()

	client, err := net.DialUDP("udp", nil, listener.LocalAddr().(*net.UDPAddr))
	if err != nil {
		cancel()
		t.Fatalf("DialUDP() error = %v", err)
	}
	defer client.Close()
	if err := client.SetDeadline(time.Now().Add(time.Second)); err != nil {
		t.Fatalf("SetDeadline() error = %v", err)
	}
	if _, err := client.Write([]byte(Request)); err != nil {
		t.Fatalf("Write() error = %v", err)
	}
	buffer := make([]byte, 1200)
	count, err := client.Read(buffer)
	if err != nil {
		t.Fatalf("Read() error = %v", err)
	}
	want := fmt.Sprintf("%s %d %s", Response, 17806, spki)
	if got := string(buffer[:count]); got != want {
		t.Fatalf("discovery response = %q, want %q", got, want)
	}
	if strings.Contains(string(buffer[:count]), "token") || strings.Contains(string(buffer[:count]), "secret") {
		t.Fatalf("discovery response contains a secret field: %s", buffer[:count])
	}

	cancel()
	select {
	case err := <-done:
		if err != nil {
			t.Fatalf("serveConnection() error = %v", err)
		}
	case <-time.After(time.Second):
		t.Fatal("discovery responder did not stop with context")
	}
}

func TestNewResponderRejectsInvalidSPKI(t *testing.T) {
	_, err := NewResponder(Config{
		BindAddress: "127.0.0.1", UDPPort: 17807, WSSPort: 17806,
	}, "not-a-digest")
	if err == nil {
		t.Fatal("NewResponder() accepted invalid SPKI")
	}
}
