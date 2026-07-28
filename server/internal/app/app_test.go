package app

import (
	"bytes"
	"context"
	"net"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/lst92991-eng/boomPI/server/internal/config"
	"github.com/lst92991-eng/boomPI/server/internal/logging"
)

func TestRunStopsWhenContextIsCanceled(t *testing.T) {
	t.Setenv("DASHSCOPE_API_KEY", "never-log-this-secret")
	t.Setenv("DASHSCOPE_WORKSPACE_ID", "test-workspace")
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
	cancel()
	select {
	case err := <-result:
		if err != nil {
			t.Fatalf("Run() error = %v", err)
		}
	case <-time.After(time.Second):
		t.Fatal("Run() did not stop after context cancellation")
	}
	if strings.Contains(output.String(), "never-log-this-secret") {
		t.Fatal("application log leaked the API key")
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
	if bytes.Contains(w.buffer.Bytes(), []byte("boomPI server started")) {
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

func (w *startupWriter) String() string {
	w.mu.Lock()
	defer w.mu.Unlock()
	return w.buffer.String()
}
