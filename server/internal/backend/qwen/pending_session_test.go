package qwen

import (
	"context"
	"errors"
	"sync"
	"testing"
	"time"

	"github.com/lst92991-eng/boomPI/server/internal/backend"
)

func TestPendingSessionGivesFirstMediaABoundedReadinessGrace(t *testing.T) {
	releaseOpen := make(chan struct{})
	readySession := newPendingTestSession()
	session := newPendingSession(context.Background(), func(context.Context) (backend.ConversationSession, error) {
		<-releaseOpen
		return readySession, nil
	})
	defer session.Close()

	// No input reached Qwen, so cancelling or discarding this local turn is a
	// successful no-op and must not wait for the remote handshake.
	if err := session.Cancel(context.Background()); err != nil {
		t.Fatalf("Cancel() before provider readiness error = %v", err)
	}
	if err := session.DiscardLastResponse(context.Background()); err != nil {
		t.Fatalf("DiscardLastResponse() before provider readiness error = %v", err)
	}

	sent := make(chan error, 1)
	go func() { sent <- session.SendAudio(context.Background(), []byte{1, 2}) }()
	select {
	case err := <-sent:
		t.Fatalf("SendAudio() returned before provider readiness: %v", err)
	case <-time.After(25 * time.Millisecond):
	}
	close(releaseOpen)
	awaitPendingReady(t, session)
	if err := <-sent; err != nil {
		t.Fatalf("SendAudio() after provider readiness error = %v", err)
	}
	if err := session.Commit(context.Background()); err != nil {
		t.Fatalf("Commit() after provider readiness error = %v", err)
	}
	if got := readySession.audioBytes(); got != 2 {
		t.Fatalf("provider audio bytes = %d, want 2", got)
	}
}

func TestPendingSessionMediaWaitIsBounded(t *testing.T) {
	session := newPendingSession(context.Background(), func(ctx context.Context) (backend.ConversationSession, error) {
		<-ctx.Done()
		return nil, context.Cause(ctx)
	})
	defer session.Close()

	started := time.Now()
	err := session.SendAudio(context.Background(), []byte{1, 2})
	elapsed := time.Since(started)
	if !errors.Is(err, ErrSessionNotReady) {
		t.Fatalf("SendAudio() after readiness budget error = %v, want not ready", err)
	}
	if elapsed < mediaReadyWait || elapsed > 2*mediaReadyWait {
		t.Fatalf("SendAudio() readiness wait = %s, want a bounded wait near %s", elapsed, mediaReadyWait)
	}
}

func TestPendingSessionCloseCancelsProviderHandshake(t *testing.T) {
	openStarted := make(chan struct{})
	openStopped := make(chan struct{})
	session := newPendingSession(context.Background(), func(ctx context.Context) (backend.ConversationSession, error) {
		close(openStarted)
		<-ctx.Done()
		close(openStopped)
		return nil, context.Cause(ctx)
	})

	select {
	case <-openStarted:
	case <-time.After(time.Second):
		t.Fatal("provider handshake did not start")
	}
	closed := make(chan error, 1)
	go func() { closed <- session.Close() }()
	select {
	case err := <-closed:
		if err != nil {
			t.Fatalf("Close() error = %v", err)
		}
	case <-time.After(time.Second):
		t.Fatal("Close() did not cancel and join provider handshake")
	}
	select {
	case <-openStopped:
	default:
		t.Fatal("Close() returned before provider handshake stopped")
	}
	if err := session.SendAudio(context.Background(), []byte{1, 2}); !errors.Is(err, ErrSessionClosed) {
		t.Fatalf("SendAudio() after Close() error = %v, want session closed", err)
	}
	if _, ok := <-session.Events(); ok {
		t.Fatal("Events() remained open after Close()")
	}
}

func awaitPendingReady(t *testing.T, session *pendingSession) {
	t.Helper()
	select {
	case <-session.ready:
	case <-time.After(time.Second):
		t.Fatal("provider session did not become ready")
	}
}

type pendingTestSession struct {
	mu        sync.Mutex
	audio     int
	events    chan backend.ConversationEvent
	closeOnce sync.Once
}

func newPendingTestSession() *pendingTestSession {
	return &pendingTestSession{events: make(chan backend.ConversationEvent)}
}

func (s *pendingTestSession) SendAudio(_ context.Context, pcm []byte) error {
	s.mu.Lock()
	s.audio += len(pcm)
	s.mu.Unlock()
	return nil
}

func (*pendingTestSession) Commit(context.Context) error { return nil }
func (*pendingTestSession) Cancel(context.Context) error { return nil }
func (s *pendingTestSession) Events() <-chan backend.ConversationEvent {
	return s.events
}
func (s *pendingTestSession) Close() error {
	s.closeOnce.Do(func() { close(s.events) })
	return nil
}
func (s *pendingTestSession) audioBytes() int {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.audio
}
