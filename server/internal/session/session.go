package session

import (
	"context"
	"errors"
	"fmt"
	"sync"
	"time"

	"github.com/lst92991-eng/boomPI/server/internal/backend"
)

var (
	ErrClosed            = errors.New("session actor is closed")
	ErrInvalidTransition = errors.New("invalid session transition")
	ErrStaleEpoch        = errors.New("stale session epoch")
)

const (
	commandCapacity      = 32
	urgentCapacity       = 4
	eventCapacity        = 32
	pcmFrameBytes16kMono = 320 * 2
)

type Limits struct {
	IdleTimeout      time.Duration
	MaxTurns         int
	MaxContextTokens int
}

func (l Limits) Validate() error {
	if l.IdleTimeout < time.Minute || l.IdleTimeout > 24*time.Hour {
		return errors.New("session idle timeout must be between 1m and 24h")
	}
	if l.MaxTurns < 1 || l.MaxTurns > 100 {
		return errors.New("session max turns must be between 1 and 100")
	}
	if l.MaxContextTokens < 1024 || l.MaxContextTokens > 1_000_000 {
		return errors.New("session max context tokens must be between 1024 and 1000000")
	}
	return nil
}

// Event adds the device epoch to a provider-neutral response event. Consumers
// use Epoch to reject data that was already delivered when a turn was cancelled.
type Event struct {
	Epoch uint64
	backend.ConversationEvent
}

type operation uint8

const (
	startTurn operation = iota + 1
	appendPCM
	commit
	cancelTurn
)

type command struct {
	ctx    context.Context
	op     operation
	epoch  uint64
	pcm    []byte
	result chan error
}

type phase uint8

const (
	idle phase = iota
	listening
	thinking
	speaking
)

// actorState is owned exclusively by Actor.run.
type actorState struct {
	phase      phase
	latest     uint64
	epoch      uint64
	responseID string
	frames     int
	pending    *Event
}

type Actor struct {
	provider backend.ConversationSession
	commands chan command
	urgent   chan command
	events   chan Event
	done     chan struct{}
	stop     context.CancelFunc
	stopOnce sync.Once
	closeErr error
}

// Open creates the provider connection, then starts the one goroutine that
// owns all turn state.
func Open(ctx context.Context, provider backend.ConversationBackend, cfg backend.SessionConfig) (*Actor, error) {
	if ctx == nil || provider == nil {
		return nil, errors.New("context and conversation backend are required")
	}
	actorCtx, stop := context.WithCancel(ctx)
	providerSession, err := provider.Open(actorCtx, cfg)
	if err != nil {
		stop()
		return nil, fmt.Errorf("open conversation backend: %w", err)
	}
	if providerSession == nil {
		stop()
		return nil, errors.New("conversation backend returned a nil session")
	}
	a := &Actor{
		provider: providerSession,
		commands: make(chan command, commandCapacity),
		urgent:   make(chan command, urgentCapacity),
		events:   make(chan Event, eventCapacity),
		done:     make(chan struct{}),
		stop:     stop,
	}
	go a.run(actorCtx)
	return a, nil
}

func (a *Actor) StartTurn(ctx context.Context, epoch uint64) error {
	return a.send(ctx, startTurn, epoch, nil)
}

// AppendPCM copies one 20 ms, 16 kHz S16_LE mono frame before returning.
func (a *Actor) AppendPCM(ctx context.Context, epoch uint64, pcm []byte) error {
	if len(pcm) != pcmFrameBytes16kMono {
		return fmt.Errorf("PCM frame must contain %d bytes", pcmFrameBytes16kMono)
	}
	return a.send(ctx, appendPCM, epoch, append([]byte(nil), pcm...))
}

func (a *Actor) Commit(ctx context.Context, epoch uint64) error {
	return a.send(ctx, commit, epoch, nil)
}

// Cancel is urgent and idempotent for an epoch that has already retired.
func (a *Actor) Cancel(ctx context.Context, epoch uint64) error {
	return a.send(ctx, cancelTurn, epoch, nil)
}

func (a *Actor) Events() <-chan Event { return a.events }

func (a *Actor) Close() error {
	a.stopOnce.Do(a.stop)
	<-a.done
	return a.closeErr
}

func (a *Actor) send(ctx context.Context, op operation, epoch uint64, pcm []byte) error {
	if ctx == nil {
		return errors.New("context is required")
	}
	if epoch == 0 {
		return errors.New("epoch must be non-zero")
	}
	cmd := command{ctx: ctx, op: op, epoch: epoch, pcm: pcm, result: make(chan error, 1)}
	queue := a.commands
	if op == cancelTurn {
		queue = a.urgent
	}
	select {
	case queue <- cmd:
	case <-ctx.Done():
		return ctx.Err()
	case <-a.done:
		return ErrClosed
	}
	select {
	case err := <-cmd.result:
		return err
	case <-ctx.Done():
		return ctx.Err()
	case <-a.done:
		return ErrClosed
	}
}

func (a *Actor) run(ctx context.Context) {
	state := actorState{phase: idle}
	defer a.shutdown(&state)

	for {
		// Give barge-in cancellation priority over queued PCM.
		select {
		case cmd := <-a.urgent:
			cmd.result <- a.apply(cmd, &state)
			continue
		default:
		}

		var providerEvents <-chan backend.ConversationEvent
		if state.pending == nil {
			providerEvents = a.provider.Events()
		}
		var output chan<- Event
		var next Event
		if state.pending != nil {
			output, next = a.events, *state.pending
		}

		select {
		case <-ctx.Done():
			return
		case cmd := <-a.urgent:
			cmd.result <- a.apply(cmd, &state)
		case cmd := <-a.commands:
			if err := cmd.ctx.Err(); err != nil {
				cmd.result <- err
			} else {
				cmd.result <- a.apply(cmd, &state)
			}
		case raw, ok := <-providerEvents:
			if !ok {
				a.reportClosedStream(&state)
				return
			}
			state.pending = accept(raw, &state)
		case output <- next:
			state.pending = nil
		}
	}
}

func (a *Actor) apply(cmd command, state *actorState) error {
	switch cmd.op {
	case startTurn:
		if state.phase != idle {
			return ErrInvalidTransition
		}
		if cmd.epoch <= state.latest {
			return ErrStaleEpoch
		}
		state.phase, state.latest, state.epoch = listening, cmd.epoch, cmd.epoch
		state.responseID, state.frames = "", 0
		return nil

	case appendPCM:
		if stale(cmd.epoch, state) {
			return ErrStaleEpoch
		}
		if state.phase != listening || cmd.epoch != state.epoch {
			return ErrInvalidTransition
		}
		if err := a.provider.SendAudio(cmd.ctx, cmd.pcm); err != nil {
			return fmt.Errorf("send audio: %w", err)
		}
		state.frames++
		return nil

	case commit:
		if stale(cmd.epoch, state) {
			return ErrStaleEpoch
		}
		if state.phase != listening || cmd.epoch != state.epoch || state.frames == 0 {
			return ErrInvalidTransition
		}
		if err := a.provider.Commit(cmd.ctx); err != nil {
			return fmt.Errorf("commit audio: %w", err)
		}
		state.phase = thinking
		return nil

	case cancelTurn:
		if state.phase == idle {
			if cmd.epoch < state.latest {
				return nil
			}
			if cmd.epoch != state.latest {
				return ErrInvalidTransition
			}
			if discarder, ok := a.provider.(backend.CompletedResponseDiscarder); ok {
				if err := discarder.DiscardLastResponse(cmd.ctx); err != nil {
					return fmt.Errorf("discard completed response: %w", err)
				}
			}
			return nil
		}
		if cmd.epoch < state.epoch {
			return nil
		}
		if cmd.epoch != state.epoch {
			return ErrInvalidTransition
		}
		responseWasActive := state.phase == thinking || state.phase == speaking
		if err := a.provider.Cancel(cmd.ctx); err != nil {
			return fmt.Errorf("cancel response: %w", err)
		}
		if responseWasActive {
			if discarder, ok := a.provider.(backend.CompletedResponseDiscarder); ok {
				if err := discarder.DiscardLastResponse(cmd.ctx); err != nil {
					return fmt.Errorf("discard cancelled response: %w", err)
				}
			}
		}
		a.drainProviderEvents()
		if state.pending != nil && state.pending.Epoch == state.epoch {
			state.pending = nil
		}
		state.phase, state.epoch, state.responseID, state.frames = idle, 0, "", 0
		return nil
	}
	return errors.New("unknown session operation")
}

// Cancel establishes a provider fence first, so every event left in the
// provider channel belongs to the retired epoch and can be drained safely.
func (a *Actor) drainProviderEvents() {
	for {
		select {
		case _, ok := <-a.provider.Events():
			if !ok {
				return
			}
		default:
			return
		}
	}
}

func stale(epoch uint64, state *actorState) bool {
	return epoch < state.epoch || (state.phase == idle && epoch <= state.latest)
}

func accept(raw backend.ConversationEvent, state *actorState) *Event {
	if state.phase != thinking && state.phase != speaking {
		return nil
	}
	if err := validProviderEvent(raw); err != nil {
		state.phase, state.responseID = idle, ""
		return &Event{Epoch: state.epoch, ConversationEvent: backend.ConversationEvent{Type: backend.EventError, Err: err}}
	}
	if state.responseID == "" {
		state.responseID = raw.ResponseID
	}
	if raw.ResponseID != "" && raw.ResponseID != state.responseID {
		return nil
	}
	raw.PCM = append([]byte(nil), raw.PCM...)
	event := &Event{Epoch: state.epoch, ConversationEvent: raw}
	if raw.Type == backend.EventAudio {
		state.phase = speaking
	}
	if raw.Type == backend.EventDone || raw.Type == backend.EventError {
		state.phase, state.responseID = idle, ""
	}
	return event
}

func validProviderEvent(event backend.ConversationEvent) error {
	switch event.Type {
	case backend.EventStarted:
		if event.ResponseID != "" {
			return nil
		}
	case backend.EventTextDelta:
		if event.ResponseID != "" && event.Text != "" {
			return nil
		}
	case backend.EventAudio:
		if event.ResponseID != "" && len(event.PCM) > 0 && event.SampleRateHz > 0 {
			return nil
		}
	case backend.EventDone:
		if event.ResponseID != "" {
			return nil
		}
	case backend.EventError:
		if event.Err != nil {
			return nil
		}
	}
	return fmt.Errorf("invalid provider event type %d", event.Type)
}

func (a *Actor) reportClosedStream(state *actorState) {
	if state.phase == idle {
		return
	}
	event := Event{Epoch: state.epoch, ConversationEvent: backend.ConversationEvent{
		Type: backend.EventError, Err: errors.New("conversation backend event stream closed"),
	}}
	select {
	case a.events <- event:
	default:
	}
}

func (a *Actor) shutdown(state *actorState) {
	if state.phase != idle {
		cancelCtx, cancel := context.WithTimeout(context.Background(), time.Second)
		if err := a.provider.Cancel(cancelCtx); err != nil && !errors.Is(err, context.Canceled) {
			a.closeErr = fmt.Errorf("cancel active turn: %w", err)
		}
		cancel()
	}
	if err := a.provider.Close(); err != nil && a.closeErr == nil {
		a.closeErr = fmt.Errorf("close conversation backend: %w", err)
	}
	close(a.events)
	close(a.done)
}
