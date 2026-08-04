package qwen

import (
	"context"
	"errors"
	"sync"
	"sync/atomic"
	"time"

	"github.com/lst92991-eng/boomPI/server/internal/backend"
)

var ErrSessionNotReady = errors.New("qwen session is still connecting")

// The device uplink queue holds about 640 ms of 20 ms frames. Waiting less
// than that lets a nearly-complete provider handshake preserve the first turn
// without allowing an unavailable provider to fill the transport queue.
const mediaReadyWait = 500 * time.Millisecond

// pendingSession keeps provider setup off the authenticated device hello path.
// Media arriving just before Qwen is ready receives one bounded grace period;
// no command or audio is accumulated in this adapter.
type pendingSession struct {
	ctx    context.Context
	cancel context.CancelFunc

	ready  chan struct{}
	events chan backend.ConversationEvent
	done   chan struct{}
	closed atomic.Bool

	mu        sync.RWMutex
	session   backend.ConversationSession
	openErr   error
	closeErr  error
	closeOnce sync.Once
}

var _ backend.ConversationSession = (*pendingSession)(nil)
var _ backend.CompletedResponseDiscarder = (*pendingSession)(nil)

func newPendingSession(
	parent context.Context,
	open func(context.Context) (backend.ConversationSession, error),
) *pendingSession {
	ctx, cancel := context.WithCancel(parent)
	session := &pendingSession{
		ctx: ctx, cancel: cancel,
		ready: make(chan struct{}), events: make(chan backend.ConversationEvent), done: make(chan struct{}),
	}
	go session.run(open)
	return session
}

func (s *pendingSession) run(open func(context.Context) (backend.ConversationSession, error)) {
	defer close(s.done)
	defer close(s.events)

	providerSession, err := open(s.ctx)
	if err == nil && providerSession == nil {
		err = errors.New("qwen backend returned a nil session")
	}
	s.mu.Lock()
	s.session = providerSession
	s.openErr = err
	close(s.ready)
	s.mu.Unlock()
	if err != nil {
		return
	}

	defer func() {
		err := providerSession.Close()
		s.mu.Lock()
		s.closeErr = err
		s.mu.Unlock()
	}()
	for {
		select {
		case <-s.ctx.Done():
			return
		case event, ok := <-providerSession.Events():
			if !ok {
				return
			}
			select {
			case s.events <- event:
			case <-s.ctx.Done():
				return
			}
		}
	}
}

func (s *pendingSession) Events() <-chan backend.ConversationEvent { return s.events }

func (s *pendingSession) SendAudio(ctx context.Context, pcm []byte) error {
	providerSession, err := s.readySession(ctx, mediaReadyWait)
	if err != nil {
		return err
	}
	return providerSession.SendAudio(ctx, pcm)
}

func (s *pendingSession) Commit(ctx context.Context) error {
	providerSession, err := s.readySession(ctx, mediaReadyWait)
	if err != nil {
		return err
	}
	return providerSession.Commit(ctx)
}

func (s *pendingSession) Cancel(ctx context.Context) error {
	providerSession, err := s.readySession(ctx, 0)
	if errors.Is(err, ErrSessionNotReady) {
		// No audio has reached Qwen yet, so there is no remote turn to fence.
		return nil
	}
	if err != nil {
		return err
	}
	return providerSession.Cancel(ctx)
}

func (s *pendingSession) DiscardLastResponse(ctx context.Context) error {
	providerSession, err := s.readySession(ctx, 0)
	if errors.Is(err, ErrSessionNotReady) {
		return nil
	}
	if err != nil {
		return err
	}
	discarder, ok := providerSession.(backend.CompletedResponseDiscarder)
	if !ok {
		return nil
	}
	return discarder.DiscardLastResponse(ctx)
}

func (s *pendingSession) readySession(ctx context.Context, wait time.Duration) (backend.ConversationSession, error) {
	if ctx == nil {
		ctx = context.Background()
	}
	if err := context.Cause(ctx); err != nil {
		return nil, transportError(err)
	}
	if s.closed.Load() {
		return nil, ErrSessionClosed
	}
	if wait <= 0 {
		select {
		case <-s.ready:
		default:
			return nil, ErrSessionNotReady
		}
	} else {
		timer := time.NewTimer(wait)
		defer timer.Stop()
		select {
		case <-s.ready:
		case <-ctx.Done():
			return nil, transportError(context.Cause(ctx))
		case <-s.ctx.Done():
			if s.closed.Load() {
				return nil, ErrSessionClosed
			}
			return nil, transportError(context.Cause(s.ctx))
		case <-timer.C:
			return nil, ErrSessionNotReady
		}
	}
	s.mu.RLock()
	providerSession, err := s.session, s.openErr
	s.mu.RUnlock()
	if err != nil {
		return nil, err
	}
	if providerSession == nil {
		return nil, ErrSessionClosed
	}
	return providerSession, nil
}

func (s *pendingSession) Close() error {
	s.closeOnce.Do(func() {
		s.closed.Store(true)
		s.cancel()
	})
	<-s.done
	s.mu.RLock()
	err := s.closeErr
	s.mu.RUnlock()
	return err
}
