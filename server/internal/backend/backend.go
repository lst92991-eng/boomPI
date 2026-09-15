package backend

import (
	"context"
)

type SessionConfig struct {
	DeviceID     string
	SystemPrompt string
	Persona      string
}

type EventType uint8

const (
	EventUnknown EventType = iota
	EventStarted
	EventTextDelta
	EventAudio
	EventDone
	EventError
)

// ConversationEvent is the provider-neutral output of one response. Text and
// PCM are deltas; callers must process events in channel order.
type ConversationEvent struct {
	Type         EventType
	ResponseID   string
	Text         string
	PCM          []byte
	SampleRateHz int
	Err          error
}

type ConversationSession interface {
	// SendAudio, Commit, and Cancel must honor ctx and use bounded provider
	// queues; the session actor must never wait on an unbounded network write.
	SendAudio(ctx context.Context, pcm []byte) error
	Commit(ctx context.Context) error
	// Cancel stops active work and clears incomplete input, preserving completed
	// history unless retract explicitly removes the most recent unheard response.
	Cancel(ctx context.Context, retract bool) error
	// Events returns one stable, provider-owned bounded channel. Cancel must fence the
	// cancelled response before returning: already queued events may remain,
	// but the provider must not enqueue more events for that response.
	Events() <-chan ConversationEvent
	Close() error
}

type ConversationBackend interface {
	Open(ctx context.Context, cfg SessionConfig) (ConversationSession, error)
}
