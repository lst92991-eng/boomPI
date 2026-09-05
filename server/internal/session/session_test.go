package session

import (
	"context"
	"fmt"
	"sync"
	"sync/atomic"
	"testing"
	"time"

	"github.com/lst92991-eng/boomPI/server/internal/backend"
	"github.com/lst92991-eng/boomPI/server/internal/protocol"
)

type fakeProvider struct {
	events     chan backend.ConversationEvent
	sent       chan []byte
	cancelHook func(context.Context) error
	discarded  atomic.Int32
	committed  atomic.Int32
	once       sync.Once
}

func newFake() *fakeProvider {
	return &fakeProvider{events: make(chan backend.ConversationEvent, 32), sent: make(chan []byte, 32)}
}
func (p *fakeProvider) Open(context.Context, backend.SessionConfig) (backend.ConversationSession, error) {
	return p, nil
}
func (p *fakeProvider) SendAudio(ctx context.Context, pcm []byte) error {
	select {
	case p.sent <- append([]byte(nil), pcm...):
		return nil
	case <-ctx.Done():
		return ctx.Err()
	}
}
func (p *fakeProvider) Commit(context.Context) error { p.committed.Add(1); return nil }
func (p *fakeProvider) Cancel(ctx context.Context) error {
	if p.cancelHook != nil {
		return p.cancelHook(ctx)
	}
	return nil
}
func (p *fakeProvider) DiscardLastResponse(context.Context) error { p.discarded.Add(1); return nil }
func (p *fakeProvider) Events() <-chan backend.ConversationEvent  { return p.events }
func (p *fakeProvider) Close() error                              { p.once.Do(func() { close(p.events) }); return nil }
func submit(t *testing.T, a *Actor, generation, sequence uint32, flags uint16) {
	t.Helper()
	if err := a.Submit(protocol.PCMHeader{Generation: generation, Sequence: sequence, Flags: flags}, make([]byte, 640)); err != nil {
		t.Fatal(err)
	}
}
func waitSent(t *testing.T, p *fakeProvider) {
	t.Helper()
	select {
	case <-p.sent:
	case <-time.After(time.Second):
		t.Fatal("provider input not received")
	}
}

func TestSupersedeQueueAcceptsNewPCMWhileProviderCancellationBlocks(t *testing.T) {
	p := newFake()
	a, err := Open(context.Background(), p, backend.SessionConfig{})
	if err != nil {
		t.Fatal(err)
	}
	defer a.Close()
	submit(t, a, 1, 0, 3)
	waitSent(t, p)
	// First event proves Commit completed and the worker is in response mode.
	p.events <- backend.ConversationEvent{Type: backend.EventStarted, ResponseID: "old"}
	select {
	case <-a.Events():
	case <-time.After(time.Second):
		t.Fatal("no start")
	}
	entered, release := make(chan struct{}), make(chan struct{})
	p.cancelHook = func(ctx context.Context) error {
		close(entered)
		select {
		case <-release:
			return nil
		case <-ctx.Done():
			return ctx.Err()
		}
	}
	submit(t, a, 2, 0, 5)
	select {
	case <-entered:
	case <-time.After(time.Second):
		t.Fatal("no cancellation")
	}
	for i := uint32(1); i <= 20; i++ {
		flags := uint16(0)
		if i == 20 {
			flags = 2
		}
		submit(t, a, 2, i, flags)
	}
	// These old events existed before Cancel returned. They must be drained.
	p.events <- backend.ConversationEvent{Type: backend.EventTextDelta, ResponseID: "old", Text: "stale"}
	p.events <- backend.ConversationEvent{Type: backend.EventDone, ResponseID: "old"}
	close(release)
	for i := 0; i < 21; i++ {
		waitSent(t, p)
	}
	if p.discarded.Load() != 1 {
		t.Fatal("retraction missing")
	}
	p.events <- backend.ConversationEvent{Type: backend.EventStarted, ResponseID: "new"}
	select {
	case event := <-a.Events():
		if event.Generation != 2 || event.ResponseID != "new" {
			t.Fatalf("stale event %+v", event)
		}
	case <-time.After(time.Second):
		t.Fatal("new generation did not resume")
	}
}

func TestBoundaryQueueBoundAndRetractionSurvivesFollowingNormal(t *testing.T) {
	p := newFake()
	entered, release := make(chan struct{}), make(chan struct{})
	var once sync.Once
	p.cancelHook = func(ctx context.Context) error {
		once.Do(func() { close(entered) })
		select {
		case <-release:
			return nil
		case <-ctx.Done():
			return ctx.Err()
		}
	}
	a, err := Open(context.Background(), p, backend.SessionConfig{})
	if err != nil {
		t.Fatal(err)
	}
	defer a.Close()
	submit(t, a, 1, 0, 1)
	<-entered
	submit(t, a, 2, 0, 5)
	submit(t, a, 3, 0, 3)
	if err = a.Stop(4, false); err != ErrCongested {
		t.Fatalf("unbounded boundary queue: %v", err)
	}
	close(release)
	// Failed enqueue invalidates this actor's generation; connection owner
	// closes it. No partial utterance is ever committed after queue failure.
	select {
	case <-p.sent:
		t.Fatal("failed queue admitted PCM")
	case <-time.After(30 * time.Millisecond):
	}
	if p.committed.Load() != 0 {
		t.Fatal("partial input committed")
	}
}

func TestPCMQueueIsBoundedAndStopRetainsReservedCapacity(t *testing.T) {
	p := newFake()
	entered, release := make(chan struct{}), make(chan struct{})
	var once sync.Once
	p.cancelHook = func(ctx context.Context) error {
		once.Do(func() { close(entered) })
		select {
		case <-release:
			return nil
		case <-ctx.Done():
			return ctx.Err()
		}
	}
	a, err := Open(context.Background(), p, backend.SessionConfig{})
	if err != nil {
		t.Fatal(err)
	}
	defer a.Close()
	submit(t, a, 1, 0, 1)
	<-entered
	for i := 1; i <= commandCapacity; i++ {
		submit(t, a, 1, uint32(i), 0)
	}
	if err = a.Submit(protocol.PCMHeader{Generation: 1, Sequence: 25}, make([]byte, 640)); err != ErrCongested {
		t.Fatal("unbounded PCM queue")
	}
	if err = a.Stop(2, false); err != nil {
		t.Fatal("STOP capacity unavailable")
	}
	close(release)
}

func TestCancellationDeadlineClosesSessionWithoutSpawningReplacementWorkers(t *testing.T) {
	p := newFake()
	p.cancelHook = func(ctx context.Context) error { <-ctx.Done(); return ctx.Err() }
	a, err := Open(context.Background(), p, backend.SessionConfig{})
	if err != nil {
		t.Fatal(err)
	}
	defer a.Close()
	submit(t, a, 1, 0, 1)
	select {
	case _, ok := <-a.Events():
		if ok {
			t.Fatal("unexpected event")
		}
	case <-time.After(time.Second):
		t.Fatal("provider cancellation was not bounded")
	}
	if p.committed.Load() != 0 {
		t.Fatal("committed without input")
	}
}

func TestNormalAndExplicitStopHistoryIntent(t *testing.T) {
	for _, retract := range []bool{false, true} {
		t.Run(fmt.Sprint(retract), func(t *testing.T) {
			p := newFake()
			a, err := Open(context.Background(), p, backend.SessionConfig{})
			if err != nil {
				t.Fatal(err)
			}
			defer a.Close()
			submit(t, a, 1, 0, 3)
			waitSent(t, p)
			if err = a.Stop(2, retract); err != nil {
				t.Fatal(err)
			}
			submit(t, a, 3, 0, 3)
			waitSent(t, p)
			want := int32(0)
			if retract {
				want = 1
			}
			if p.discarded.Load() != want {
				t.Fatal("history intent lost")
			}
		})
	}
}
