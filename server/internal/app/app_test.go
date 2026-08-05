package app

import (
	"bytes"
	"context"
	"io"
	"log/slog"
	"net"
	"strconv"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/lst92991-eng/boomPI/server/internal/backend/qwenpipeline"
	"github.com/lst92991-eng/boomPI/server/internal/config"
	"github.com/lst92991-eng/boomPI/server/internal/discovery"
	"github.com/lst92991-eng/boomPI/server/internal/logging"
)

func TestQwenBackendUsesTeachingPipeline(t *testing.T) {
	t.Setenv("DASHSCOPE_API_KEY", "offline-test-key")
	t.Setenv("DASHSCOPE_WORKSPACE_ID", "offline-test-workspace")
	cfg, err := config.Load("")
	if err != nil {
		t.Fatalf("config.Load() error = %v", err)
	}
	logger := slog.New(slog.NewTextHandler(io.Discard, nil))

	provider, err := newQwenBackend(cfg, logger)
	if err != nil {
		t.Fatalf("newQwenBackend(default) error = %v", err)
	}
	if _, ok := provider.(*qwenpipeline.Backend); !ok {
		t.Fatalf("backend type = %T, want ASR/LLM/TTS pipeline", provider)
	}
}

func TestRunStopsWhenContextIsCanceled(t *testing.T) {
	t.Setenv("DASHSCOPE_API_KEY", "never-log-this-secret")
	t.Setenv("DASHSCOPE_WORKSPACE_ID", "test-workspace")
	cfg, err := config.Load("")
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

func TestRunListenersRejectsUnexpectedCleanExit(t *testing.T) {
	peerStopped := make(chan struct{})
	err := runListeners(
		context.Background(),
		func(context.Context) error { return nil },
		func(ctx context.Context) error {
			<-ctx.Done()
			close(peerStopped)
			return nil
		},
	)
	if err == nil || !strings.Contains(err.Error(), "without a shutdown request") {
		t.Fatalf("runListeners() error = %v, want unexpected listener exit", err)
	}
	select {
	case <-peerStopped:
	default:
		t.Fatal("runListeners() did not cancel and join the peer listener")
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
