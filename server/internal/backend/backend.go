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
	// history. Retraction is explicit through CompletedResponseDiscarder.
	Cancel(ctx context.Context) error
	// Events returns one stable, provider-owned bounded channel. Cancel must fence the
	// cancelled response before returning: already queued events may remain,
	// but the provider must not enqueue more events for that response.
	Events() <-chan ConversationEvent
	Close() error
}

// CompletedResponseDiscarder is an optional provider capability used when
// playback is cancelled after inference has already committed the assistant
// response to provider-side conversation history.
type CompletedResponseDiscarder interface {
	DiscardLastResponse(ctx context.Context) error
}

type ConversationBackend interface {
	Open(ctx context.Context, cfg SessionConfig) (ConversationSession, error)
}
