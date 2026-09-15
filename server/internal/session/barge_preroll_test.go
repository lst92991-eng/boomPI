package session

import (
	"context"
	"testing"
	"time"

	"github.com/lst92991-eng/boomPI/server/internal/backend"
)

func TestFullPreRollAndLiveInputSurviveBlockedSupersede(t *testing.T) {
	p := newFake()
	a, err := Open(context.Background(), p, backend.SessionConfig{})
	if err != nil {
		t.Fatal(err)
	}
	defer a.Close()
	begin(t, a, 1, false)
	submit(t, a, 1, 0)
	finish(t, a, 1)
	waitSent(t, p)
	p.events <- backend.ConversationEvent{Type: backend.EventStarted, ResponseID: "old"}
	select {
	case <-a.Events():
	case <-time.After(time.Second):
		t.Fatal("old response did not start")
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
	begin(t, a, 2, true)
	select {
	case <-entered:
	case <-time.After(time.Second):
		t.Fatal("cancel did not start")
	}
	// 确定性模拟等待取消期间的完整突发，不依赖真实云端或收费调用。
	for i := uint32(0); i < 95; i++ {
		submit(t, a, 2, i)
	}
	finish(t, a, 2)
	close(release)
	for i := 0; i < 95; i++ {
		waitSent(t, p)
	}
	p.events <- backend.ConversationEvent{Type: backend.EventStarted, ResponseID: "new"}
	select {
	case event := <-a.Events():
		if event.Generation != 2 || event.ResponseID != "new" || p.discarded.Load() != 1 {
			t.Fatalf("supersede lost input or history intent: %+v", event)
		}
	case <-time.After(time.Second):
		t.Fatal("new input never committed")
	}
}
