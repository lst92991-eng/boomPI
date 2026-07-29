package backend

import (
	"context"
	"errors"
	"fmt"
	"sort"
	"strings"
	"sync"
)

var ErrNotRegistered = errors.New("backend is not registered")

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

type ASRBackend interface {
	Transcribe(ctx context.Context, pcm []byte) (string, error)
}

type LLMBackend interface {
	Complete(ctx context.Context, messages []string) (string, error)
}

type TTSBackend interface {
	Synthesize(ctx context.Context, text string) ([]byte, error)
}

type ConversationFactory func() (ConversationBackend, error)

type Registry struct {
	mu           sync.RWMutex
	conversation map[string]ConversationFactory
}

func NewRegistry() *Registry {
	return &Registry{conversation: make(map[string]ConversationFactory)}
}

func (r *Registry) RegisterConversation(name string, factory ConversationFactory) error {
	name = strings.TrimSpace(strings.ToLower(name))
	if name == "" || factory == nil {
		return errors.New("backend name and factory are required")
	}
	r.mu.Lock()
	defer r.mu.Unlock()
	if _, exists := r.conversation[name]; exists {
		return fmt.Errorf("conversation backend %q is already registered", name)
	}
	r.conversation[name] = factory
	return nil
}

func (r *Registry) Conversation(name string) (ConversationBackend, error) {
	r.mu.RLock()
	factory, exists := r.conversation[strings.ToLower(strings.TrimSpace(name))]
	r.mu.RUnlock()
	if !exists {
		return nil, ErrNotRegistered
	}
	return factory()
}

func (r *Registry) ConversationNames() []string {
	r.mu.RLock()
	defer r.mu.RUnlock()
	names := make([]string, 0, len(r.conversation))
	for name := range r.conversation {
		names = append(names, name)
	}
	sort.Strings(names)
	return names
}
