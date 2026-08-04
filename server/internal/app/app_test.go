package app

import (
	"bytes"
	"context"
	"net"
	"strconv"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/lst92991-eng/boomPI/server/internal/config"
	"github.com/lst92991-eng/boomPI/server/internal/discovery"
	"github.com/lst92991-eng/boomPI/server/internal/logging"
)

func TestRunStopsWhenContextIsCanceled(t *testing.T) {
	t.Setenv("DASHSCOPE_API_KEY", "never-log-this-secret")
	t.Setenv("DASHSCOPE_WORKSPACE_ID", "test-workspace")
	t.Setenv("BOOMPI_DEVICE_TOKEN", "0123456789abcdef0123456789abcdef")
	cfg, err := config.Load("", nil)
	if err != nil {
		t.Fatalf("config.Load() error = %v", err)
	}
	output := newStartupWriter()
	logger, err := logging.NewJSON(output, "debug")
	if err != nil {
		t.Fatalf("logging.NewJSON() error = %v", err)
	}
	cfg.WSSPort = freePort(t)
	cfg.DiscoveryPort = freeUDPPort(t)
	application, err := New(cfg, logger, t.TempDir())
	if err != nil {
		t.Fatalf("New() error = %v", err)
	}
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	result := make(chan error, 1)
	go func() {
		result <- application.Run(ctx)
	}()

	select {
	case <-output.started:
	case <-time.After(time.Second):
		t.Fatal("Run() did not start before timeout")
	}
	advertisedPort, advertisedSPKI := requestDiscovery(t, cfg.DiscoveryPort)
	if advertisedPort != cfg.WSSPort || advertisedSPKI == "" {
		t.Fatalf("discovery response = port %d, SPKI %q", advertisedPort, advertisedSPKI)
	}
	cancel()
	select {
	case err := <-result:
		if err != nil {
			t.Fatalf("Run() error = %v", err)
		}
	case <-time.After(time.Second):
		t.Fatal("Run() did not stop after context cancellation")
	}
	if strings.Contains(output.String(), "never-log-this-secret") || strings.Contains(output.String(), "0123456789abcdef0123456789abcdef") {
		t.Fatal("application log leaked a credential")
	}
	if !strings.Contains(output.String(), "tls_spki_sha256") {
		t.Fatalf("TLS identity was not logged: %s", output.String())
	}
}

type startupWriter struct {
	mu      sync.Mutex
	buffer  bytes.Buffer
	started chan struct{}
	once    sync.Once
}

func newStartupWriter() *startupWriter {
	return &startupWriter{started: make(chan struct{})}
}

func (w *startupWriter) Write(data []byte) (int, error) {
	w.mu.Lock()
	defer w.mu.Unlock()
	written, err := w.buffer.Write(data)
	if bytes.Contains(w.buffer.Bytes(), []byte("boomPI server starting")) {
		w.once.Do(func() { close(w.started) })
	}
	return written, err
}

func freePort(t *testing.T) int {
	t.Helper()
	listener, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatalf("net.Listen() error = %v", err)
	}
	port := listener.Addr().(*net.TCPAddr).Port
	if err := listener.Close(); err != nil {
		t.Fatalf("listener.Close() error = %v", err)
	}
	return port
}

func freeUDPPort(t *testing.T) int {
	t.Helper()
	listener, err := net.ListenUDP("udp", &net.UDPAddr{IP: net.ParseIP("127.0.0.1")})
	if err != nil {
		t.Fatalf("net.ListenUDP() error = %v", err)
	}
	port := listener.LocalAddr().(*net.UDPAddr).Port
	if err := listener.Close(); err != nil {
		t.Fatalf("listener.Close() error = %v", err)
	}
	return port
}

func requestDiscovery(t *testing.T, port int) (int, string) {
	t.Helper()
	deadline := time.Now().Add(time.Second)
	for time.Now().Before(deadline) {
		connection, err := net.DialUDP("udp", nil, &net.UDPAddr{
			IP: net.ParseIP("127.0.0.1"), Port: port,
		})
		if err != nil {
			t.Fatalf("net.DialUDP() error = %v", err)
		}
		_ = connection.SetDeadline(time.Now().Add(100 * time.Millisecond))
		_, writeErr := connection.Write([]byte(discovery.Request))
		buffer := make([]byte, 1200)
		count, readErr := connection.Read(buffer)
		_ = connection.Close()
		if writeErr == nil && readErr == nil {
			fields := strings.Fields(string(buffer[:count]))
			if len(fields) != 3 || fields[0] != discovery.Response {
				t.Fatalf("unexpected discovery response: %q", buffer[:count])
			}
			advertisedPort, err := strconv.Atoi(fields[1])
			if err != nil {
				t.Fatalf("parse discovery port: %v", err)
			}
			return advertisedPort, fields[2]
		}
		time.Sleep(10 * time.Millisecond)
	}
	t.Fatal("UDP discovery did not answer before timeout")
	return 0, ""
}

func (w *startupWriter) String() string {
	w.mu.Lock()
	defer w.mu.Unlock()
	return w.buffer.String()
}
