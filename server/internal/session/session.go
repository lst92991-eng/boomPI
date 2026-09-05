package session

import (
	"context"
	"errors"
	"sync"
	"sync/atomic"
	"time"

	"github.com/lst92991-eng/boomPI/server/internal/backend"
	"github.com/lst92991-eng/boomPI/server/internal/protocol"
)

const (
	// 34 ordinary frames + 2 reserved STARTs + one in the provider call and
	// one in the socket reader stay below the 40-frame (800 ms) input budget.
	commandCapacity          = 34
	urgentCapacity           = 2
	eventCapacity            = 8
	providerOperationTimeout = 700 * time.Millisecond
)

var ErrCongested = errors.New("session input queue full")
var ErrClosed = errors.New("session closed")

type Event struct {
	Generation uint32
	backend.ConversationEvent
}

type command struct {
	generation                uint32
	start, stop, end, retract bool
	pcm                       []byte
	queuedAt                  time.Time
}

func (c command) boundary() bool { return c.start || c.stop }

// Actor is the sole owner of provider calls. Enqueue never waits for provider
// cancellation. One worker serializes cleanup and input; no per-barge goroutine
// or unbounded canceled-provider queue exists.
type Actor struct {
	provider backend.ConversationSession
	events   chan Event
	wake     chan struct{}
	done     chan struct{}
	stop     context.CancelFunc
	latest   atomic.Uint32
	mu       sync.Mutex
	commands []command
	closed   bool
}

func Open(ctx context.Context, provider backend.ConversationBackend, cfg backend.SessionConfig) (*Actor, error) {
	actorCtx, stop := context.WithCancel(ctx)
	session, err := provider.Open(actorCtx, cfg)
	if err != nil {
		stop()
		return nil, err
	}
	if session == nil {
		stop()
		return nil, errors.New("provider returned nil session")
	}
	a := &Actor{provider: session, events: make(chan Event, eventCapacity), wake: make(chan struct{}, 1), done: make(chan struct{}), stop: stop}
	go a.run(actorCtx)
	return a, nil
}

func (a *Actor) Submit(header protocol.PCMHeader, pcm []byte) error {
	if len(pcm) != protocol.UplinkFrameBytes {
		return errors.New("invalid session PCM frame")
	}
	return a.enqueue(command{
		generation: header.Generation, start: header.Flags&protocol.PCMFlagStart != 0,
		end: header.Flags&protocol.PCMFlagEnd != 0, retract: header.Flags&protocol.PCMFlagSupersede != 0,
		pcm: append([]byte(nil), pcm...), queuedAt: time.Now(),
	})
}

func (a *Actor) Stop(generation uint32, retract bool) error {
	return a.enqueue(command{generation: generation, stop: true, retract: retract, queuedAt: time.Now()})
}

func (a *Actor) Events() <-chan Event { return a.events }

func (a *Actor) Close() error {
	a.stop()
	<-a.done
	return nil
}

func (a *Actor) enqueue(cmd command) error {
	a.mu.Lock()
	defer a.mu.Unlock()
	if a.closed {
		return ErrClosed
	}
	if cmd.boundary() {
		if cmd.generation <= a.latest.Load() {
			return errors.New("generation must increase")
		}
		a.latest.Store(cmd.generation)
		// Boundary commands retain their retract intent until the worker applies
		// them. A later NORMAL must not erase a previously requested SUPERSEDE.
		kept := a.commands[:0]
		for _, queued := range a.commands {
			if queued.boundary() {
				kept = append(kept, queued)
			}
		}
		clear(a.commands[len(kept):])
		a.commands = kept
	}
	pcm, urgent := 0, 0
	for _, queued := range a.commands {
		if queued.boundary() {
			urgent++
		} else {
			pcm++
		}
	}
	if (cmd.boundary() && urgent >= urgentCapacity) || (!cmd.boundary() && pcm >= commandCapacity) {
		return ErrCongested
	}
	a.commands = append(a.commands, cmd)
	select {
	case a.wake <- struct{}{}:
	default:
	}
	return nil
}

func (a *Actor) pop() (command, bool) {
	a.mu.Lock()
	defer a.mu.Unlock()
	if len(a.commands) == 0 {
		return command{}, false
	}
	cmd := a.commands[0]
	a.commands[0] = command{}
	a.commands = a.commands[1:]
	return cmd, true
}

func (a *Actor) run(ctx context.Context) {
	defer func() {
		a.stop()
		a.mu.Lock()
		a.closed = true
		clear(a.commands)
		a.commands = nil
		a.mu.Unlock()
		_ = a.provider.Close()
		close(a.events)
		close(a.done)
	}()
	var generation uint32
	var responseID string
	responding := false
	var pending *Event
	for {
		if ctx.Err() != nil {
			return
		}
		if cmd, ok := a.pop(); ok {
			if cmd.boundary() {
				// Cancel guarantees no more old events after it returns; draining
				// here is safe before the new generation can produce any output.
				opCtx, cancel := context.WithTimeout(ctx, providerOperationTimeout)
				err := a.provider.Cancel(opCtx)
				if err == nil && cmd.retract {
					discarder, ok := a.provider.(backend.CompletedResponseDiscarder)
					if !ok {
						err = errors.New("provider cannot retract completed response")
					} else {
						err = discarder.DiscardLastResponse(opCtx)
					}
				}
				cancel()
				if err != nil {
					return
				}
				if !a.drainProviderEvents() {
					return
				}
				pending = nil
				responseID = ""
				responding = false
				generation = cmd.generation
			}
			if cmd.stop || cmd.generation != a.latest.Load() {
				continue
			}
			if cmd.generation != generation || responding || time.Since(cmd.queuedAt) > 800*time.Millisecond {
				return
			}
			opCtx, cancel := context.WithTimeout(ctx, providerOperationTimeout)
			err := a.provider.SendAudio(opCtx, cmd.pcm)
			if err == nil && cmd.end {
				err = a.provider.Commit(opCtx)
				responding = err == nil
			}
			cancel()
			if err != nil {
				return
			}
			continue
		}
		if pending != nil && pending.Generation != a.latest.Load() {
			pending = nil
		}
		var output chan Event
		var next Event
		providerEvents := a.provider.Events()
		if pending != nil {
			output, next = a.events, *pending
			providerEvents = nil
		}
		select {
		case <-ctx.Done():
			return
		case <-a.wake:
		case output <- next:
			pending = nil
		case raw, ok := <-providerEvents:
			if !ok {
				return
			}
			if !responding || generation != a.latest.Load() {
				continue
			}
			if raw.Type == backend.EventStarted {
				if responseID != "" || raw.ResponseID == "" {
					return
				}
				responseID = raw.ResponseID
			} else if responseID == "" || raw.ResponseID != responseID {
				return
			}
			if !validProviderEvent(raw) {
				return
			}
			pending = &Event{Generation: generation, ConversationEvent: raw}
			if raw.Type == backend.EventDone || raw.Type == backend.EventError {
				responding = false
			}
		}
	}
}

func (a *Actor) drainProviderEvents() bool {
	for {
		select {
		case _, ok := <-a.provider.Events():
			if !ok {
				return false
			}
		default:
			return true
		}
	}
}

func validProviderEvent(event backend.ConversationEvent) bool {
	switch event.Type {
	case backend.EventStarted, backend.EventDone:
		return event.ResponseID != ""
	case backend.EventTextDelta:
		return len(event.Text) > 0 && len(event.Text) <= 64*1024
	case backend.EventAudio:
		return event.SampleRateHz == 24_000 && len(event.PCM) > 0 && len(event.PCM) <= protocol.DownlinkFrameBytes && len(event.PCM)%2 == 0
	case backend.EventError:
		return event.Err != nil
	}
	return false
}
