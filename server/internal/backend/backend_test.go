package backend

import (
	"context"
	"errors"
	"testing"
)

type testConversationBackend struct{}

func (testConversationBackend) Open(context.Context, SessionConfig) (ConversationSession, error) {
	return nil, nil
}

func TestRegistryRegistersAndLooksUpConversationBackend(t *testing.T) {
	registry := NewRegistry()
	if err := registry.RegisterConversation(" QWEN ", func() (ConversationBackend, error) {
		return testConversationBackend{}, nil
	}); err != nil {
		t.Fatalf("RegisterConversation() error = %v", err)
	}
	if _, err := registry.Conversation("qwen"); err != nil {
		t.Fatalf("Conversation() error = %v", err)
	}
	if names := registry.ConversationNames(); len(names) != 1 || names[0] != "qwen" {
		t.Fatalf("ConversationNames() = %v", names)
	}
	if err := registry.RegisterConversation("qwen", func() (ConversationBackend, error) {
		return testConversationBackend{}, nil
	}); err == nil {
		t.Fatal("RegisterConversation() accepted a duplicate")
	}
	if _, err := registry.Conversation("missing"); !errors.Is(err, ErrNotRegistered) {
		t.Fatalf("Conversation() missing error = %v", err)
	}
}
