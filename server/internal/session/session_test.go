package session

import (
	"context"
	"errors"
	"sync"
	"testing"
	"time"

	"github.com/lst92991-eng/boomPI/server/internal/backend"
)

func TestLimitsValidate(t *testing.T) {
	limits := Limits{IdleTimeout: 30 * time.Minute, MaxTurns: 20, MaxContextTokens: 24_000}
	if err := limits.Validate(); err != nil {
		t.Fatalf("Validate() error = %v", err)
	}
	limits.MaxTurns = 0
	if err := limits.Validate(); err == nil {
		t.Fatal("Validate() accepted zero max turns")
	}
}

func TestActorForwardsOneTurnInOrder(t *testing.T) {
	provider := newFakeProvider()
	actor := openTestActor(t, context.Background(), provider)

	requireOK(t, actor.StartTurn(context.Background(), 1))
	frame := make([]byte, pcmFrameBytes16kMono)
	frame[0] = 7
	requireOK(t, actor.AppendPCM(context.Background(), 1, frame))
	frame[0] = 99
	requireOK(t, actor.Commit(context.Background(), 1))

	provider.session.emit(backend.ConversationEvent{
		Type: backend.EventStarted, ResponseID: "response-1",
	})
	provider.session.emit(backend.ConversationEvent{
		Type: backend.EventTextDelta, ResponseID: "response-1", Text: "你好",
	})
	provider.session.emit(backend.ConversationEvent{
		Type: backend.EventAudio, ResponseID: "response-1", PCM: []byte{1, 2, 3, 4}, SampleRateHz: 24_000,
	})
	provider.session.emit(backend.ConversationEvent{
		Type: backend.EventDone, ResponseID: "response-1",
	})

	started := receiveEvent(t, actor)
	if started.Epoch != 1 || started.Type != backend.EventStarted || started.ResponseID != "response-1" {
		t.Fatalf("started event = %+v", started)
	}
	text := receiveEvent(t, actor)
	if text.Epoch != 1 || text.Type != backend.EventTextDelta || text.Text != "你好" {
		t.Fatalf("text event = %+v", text)
	}
	audio := receiveEvent(t, actor)
	if audio.Epoch != 1 || audio.Type != backend.EventAudio || audio.SampleRateHz != 24_000 {
		t.Fatalf("audio event = %+v", audio)
	}
	done := receiveEvent(t, actor)
	if done.Epoch != 1 || done.Type != backend.EventDone {
		t.Fatalf("done event = %+v", done)
	}

	provider.session.mu.Lock()
	if len(provider.session.audio) != 1 || provider.session.audio[0][0] != 7 {
		t.Fatalf("provider audio copies = %v", provider.session.audio)
	}
	if provider.session.commits != 1 {
		t.Fatalf("provider commits = %d", provider.session.commits)
	}
	provider.session.mu.Unlock()

	// Done returns the actor to Idle, so the next monotonic epoch can start.
	requireOK(t, actor.StartTurn(context.Background(), 2))
	requireOK(t, actor.Cancel(context.Background(), 2))
	if err := actor.Close(); err != nil {
		t.Fatalf("Close() error = %v", err)
	}
}

func TestActorRejectsIllegalAndDuplicateTransitions(t *testing.T) {
	provider := newFakeProvider()
	actor := openTestActor(t, context.Background(), provider)
	defer actor.Close()

	frame := make([]byte, pcmFrameBytes16kMono)
	if err := actor.AppendPCM(context.Background(), 1, frame); !errors.Is(err, ErrInvalidTransition) {
		t.Fatalf("Append before StartTurn error = %v", err)
	}
	requireOK(t, actor.StartTurn(context.Background(), 1))
	if err := actor.StartTurn(context.Background(), 1); !errors.Is(err, ErrInvalidTransition) {
		t.Fatalf("duplicate StartTurn error = %v", err)
	}
	if err := actor.Commit(context.Background(), 1); !errors.Is(err, ErrInvalidTransition) {
		t.Fatalf("empty Commit error = %v", err)
	}
	if err := actor.AppendPCM(context.Background(), 2, frame); !errors.Is(err, ErrInvalidTransition) {
		t.Fatalf("future epoch Append error = %v", err)
	}
	requireOK(t, actor.AppendPCM(context.Background(), 1, frame))
	requireOK(t, actor.Commit(context.Background(), 1))
	if err := actor.Commit(context.Background(), 1); !errors.Is(err, ErrInvalidTransition) {
		t.Fatalf("duplicate Commit error = %v", err)
	}
	requireOK(t, actor.Cancel(context.Background(), 1))
	// Cancellation is idempotent for a retired epoch.
	requireOK(t, actor.Cancel(context.Background(), 1))
	if err := actor.StartTurn(context.Background(), 1); !errors.Is(err, ErrStaleEpoch) {
		t.Fatalf("stale StartTurn error = %v", err)
	}
}

func TestActorDropsStaleInputAndProviderEvents(t *testing.T) {
	provider := newFakeProvider()
	actor := openTestActor(t, context.Background(), provider)
	defer actor.Close()

	frame := make([]byte, pcmFrameBytes16kMono)
	requireOK(t, actor.StartTurn(context.Background(), 7))
	requireOK(t, actor.AppendPCM(context.Background(), 7, frame))
	requireOK(t, actor.Commit(context.Background(), 7))
	provider.session.emit(backend.ConversationEvent{
		Type: backend.EventTextDelta, ResponseID: "response-7", Text: "first",
	})
	if got := receiveEvent(t, actor); got.ResponseID != "response-7" {
		t.Fatalf("first event = %+v", got)
	}
	requireOK(t, actor.Cancel(context.Background(), 7))

	// Events queued after Cancel are a provider contract violation, but the
	// actor still drops them while there is no active epoch.
	provider.session.emit(backend.ConversationEvent{
		Type: backend.EventTextDelta, ResponseID: "response-7", Text: "late",
	})
	assertNoEvent(t, actor)

	requireOK(t, actor.StartTurn(context.Background(), 8))
	requireOK(t, actor.AppendPCM(context.Background(), 8, frame))
	requireOK(t, actor.Commit(context.Background(), 8))
	provider.session.emit(backend.ConversationEvent{
		Type: backend.EventTextDelta, ResponseID: "response-8", Text: "current",
	})
	if got := receiveEvent(t, actor); got.Epoch != 8 || got.ResponseID != "response-8" {
		t.Fatalf("current event = %+v", got)
	}
	provider.session.emit(backend.ConversationEvent{
		Type: backend.EventAudio, ResponseID: "response-7", PCM: []byte{1, 2}, SampleRateHz: 24_000,
	})
	assertNoEvent(t, actor)

	if err := actor.AppendPCM(context.Background(), 7, frame); !errors.Is(err, ErrStaleEpoch) {
		t.Fatalf("stale Append error = %v", err)
	}
}

func TestActorCancelRemainsResponsiveWhenOutputIsFull(t *testing.T) {
	provider := newFakeProvider()
	actor := openTestActor(t, context.Background(), provider)
	defer actor.Close()

	if cap(actor.commands) != commandCapacity || cap(actor.urgent) != urgentCapacity || cap(actor.events) != eventCapacity {
		t.Fatalf("unexpected channel capacities: commands=%d urgent=%d events=%d", cap(actor.commands), cap(actor.urgent), cap(actor.events))
	}

	frame := make([]byte, pcmFrameBytes16kMono)
	requireOK(t, actor.StartTurn(context.Background(), 1))
	requireOK(t, actor.AppendPCM(context.Background(), 1, frame))
	requireOK(t, actor.Commit(context.Background(), 1))
	for i := 0; i < eventCapacity+5; i++ {
		provider.session.emit(backend.ConversationEvent{
			Type: backend.EventTextDelta, ResponseID: "response-1", Text: "delta",
		})
	}
	waitOutputFull(t, actor)

	deadline, cancel := context.WithTimeout(context.Background(), 500*time.Millisecond)
	defer cancel()
	if err := actor.Cancel(deadline, 1); err != nil {
		t.Fatalf("Cancel with full output error = %v", err)
	}
	if queued := len(provider.session.events); queued != 0 {
		t.Fatalf("provider events left after cancel fence = %d", queued)
	}
}

func TestActorCancelDiscardsResponseCompletedDuringCancelFence(t *testing.T) {
	provider := newFakeProvider()
	actor := openTestActor(t, context.Background(), provider)
	defer actor.Close()

	frame := make([]byte, pcmFrameBytes16kMono)
	requireOK(t, actor.StartTurn(context.Background(), 1))
	requireOK(t, actor.AppendPCM(context.Background(), 1, frame))
	requireOK(t, actor.Commit(context.Background(), 1))
	provider.session.emit(backend.ConversationEvent{Type: backend.EventStarted, ResponseID: "response-race"})
	if got := receiveEvent(t, actor); got.Type != backend.EventStarted {
		t.Fatalf("started event = %+v", got)
	}

	provider.session.mu.Lock()
	provider.session.emitDoneOnCancel = true
	provider.session.mu.Unlock()
	requireOK(t, actor.Cancel(context.Background(), 1))
	assertNoEvent(t, actor)

	provider.session.mu.Lock()
	discards := provider.session.discards
	order := append([]string(nil), provider.session.shutdownOrder...)
	provider.session.mu.Unlock()
	if discards != 1 || len(order) != 2 || order[0] != "cancel" || order[1] != "discard" {
		t.Fatalf("cancel fence cleanup: discards=%d order=%v", discards, order)
	}

	// Cancelling only buffered user input must not remove the preceding response.
	requireOK(t, actor.StartTurn(context.Background(), 2))
	requireOK(t, actor.Cancel(context.Background(), 2))
	provider.session.mu.Lock()
	discards = provider.session.discards
	provider.session.mu.Unlock()
	if discards != 1 {
		t.Fatalf("input-only cancellation discards = %d, want 1", discards)
	}
}

func TestActorParentCancellationOrdersProviderShutdown(t *testing.T) {
	provider := newFakeProvider()
	parent, cancel := context.WithCancel(context.Background())
	actor := openTestActor(t, parent, provider)

	requireOK(t, actor.StartTurn(context.Background(), 1))
	cancel()
	select {
	case <-actor.done:
	case <-time.After(time.Second):
		t.Fatal("actor did not stop after parent cancellation")
	}

	provider.session.mu.Lock()
	cancels := provider.session.cancels
	closes := provider.session.closes
	order := append([]string(nil), provider.session.shutdownOrder...)
	provider.session.mu.Unlock()
	if cancels != 1 || closes != 1 || len(order) != 2 || order[0] != "cancel" || order[1] != "close" {
		t.Fatalf("provider shutdown: cancels=%d closes=%d order=%v", cancels, closes, order)
	}
	if err := actor.Cancel(context.Background(), 1); !errors.Is(err, ErrClosed) {
		t.Fatalf("Cancel after close error = %v", err)
	}
	if err := actor.Close(); err != nil {
		t.Fatalf("Close after parent cancellation error = %v", err)
	}
}

func TestActorValidatesCommandAndProviderEventBoundaries(t *testing.T) {
	provider := newFakeProvider()
	actor := openTestActor(t, context.Background(), provider)
	defer actor.Close()

	if err := actor.StartTurn(context.Background(), 0); err == nil {
		t.Fatalf("zero epoch error = %v", err)
	}
	if err := actor.AppendPCM(context.Background(), 1, []byte{1, 2}); err == nil {
		t.Fatalf("short PCM error = %v", err)
	}

	frame := make([]byte, pcmFrameBytes16kMono)
	requireOK(t, actor.StartTurn(context.Background(), 1))
	requireOK(t, actor.AppendPCM(context.Background(), 1, frame))
	requireOK(t, actor.Commit(context.Background(), 1))
	provider.session.emit(backend.ConversationEvent{Type: backend.EventAudio, ResponseID: "response-1", PCM: []byte{1, 2}})
	got := receiveEvent(t, actor)
	if got.Type != backend.EventError || got.Err == nil {
		t.Fatalf("invalid provider event result = %+v", got)
	}

	// A malformed provider event retires only that turn; a new epoch remains usable.
	requireOK(t, actor.StartTurn(context.Background(), 2))
}

type fakeProvider struct {
	session *fakeConversationSession
}

func newFakeProvider() *fakeProvider {
	return &fakeProvider{session: &fakeConversationSession{events: make(chan backend.ConversationEvent, eventCapacity+8)}}
}

func (p *fakeProvider) Open(context.Context, backend.SessionConfig) (backend.ConversationSession, error) {
	return p.session, nil
}

type fakeConversationSession struct {
	mu sync.Mutex

	events           chan backend.ConversationEvent
	audio            [][]byte
	commits          int
	cancels          int
	discards         int
	closes           int
	emitDoneOnCancel bool
	shutdownOrder    []string
}

func (s *fakeConversationSession) SendAudio(_ context.Context, pcm []byte) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.audio = append(s.audio, append([]byte(nil), pcm...))
	return nil
}

func (s *fakeConversationSession) Commit(context.Context) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.commits++
	return nil
}

func (s *fakeConversationSession) Cancel(context.Context) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.cancels++
	s.shutdownOrder = append(s.shutdownOrder, "cancel")
	if s.emitDoneOnCancel {
		s.events <- backend.ConversationEvent{Type: backend.EventDone, ResponseID: "response-race"}
		s.emitDoneOnCancel = false
	}
	return nil
}

func (s *fakeConversationSession) DiscardLastResponse(context.Context) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.discards++
	s.shutdownOrder = append(s.shutdownOrder, "discard")
	return nil
}

func (s *fakeConversationSession) Events() <-chan backend.ConversationEvent {
	return s.events
}

func (s *fakeConversationSession) Close() error {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.closes++
	s.shutdownOrder = append(s.shutdownOrder, "close")
	return nil
}

func (s *fakeConversationSession) emit(event backend.ConversationEvent) {
	s.events <- event
}

func openTestActor(t *testing.T, ctx context.Context, provider backend.ConversationBackend) *Actor {
	t.Helper()
	actor, err := Open(ctx, provider, backend.SessionConfig{DeviceID: "test-device"})
	if err != nil {
		t.Fatalf("Open() error = %v", err)
	}
	return actor
}

func requireOK(t *testing.T, err error) {
	t.Helper()
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
}

func receiveEvent(t *testing.T, actor *Actor) Event {
	t.Helper()
	select {
	case event, ok := <-actor.Events():
		if !ok {
			t.Fatal("actor event channel closed")
		}
		return event
	case <-time.After(time.Second):
		t.Fatal("timed out waiting for actor event")
		return Event{}
	}
}

func assertNoEvent(t *testing.T, actor *Actor) {
	t.Helper()
	select {
	case event := <-actor.Events():
		t.Fatalf("unexpected actor event: %+v", event)
	case <-time.After(25 * time.Millisecond):
	}
}

func waitOutputFull(t *testing.T, actor *Actor) {
	t.Helper()
	deadline := time.NewTimer(time.Second)
	ticker := time.NewTicker(time.Millisecond)
	defer deadline.Stop()
	defer ticker.Stop()
	for {
		if len(actor.events) == cap(actor.events) {
			return
		}
		select {
		case <-deadline.C:
			t.Fatalf("output did not fill: len=%d cap=%d", len(actor.events), cap(actor.events))
		case <-ticker.C:
		}
	}
}
