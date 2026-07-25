package tools

import (
	"context"
	"encoding/json"
	"errors"
	"testing"
)

func TestRegistryAllowsOnlyApprovedTools(t *testing.T) {
	registry := NewRegistry()
	if err := registry.Register("shell", func(context.Context, json.RawMessage) (any, error) { return nil, nil }); err == nil {
		t.Fatal("Register() unexpectedly accepted shell")
	}
	if err := registry.Register("date_time", func(context.Context, json.RawMessage) (any, error) { return "now", nil }); err != nil {
		t.Fatalf("Register() error = %v", err)
	}
	value, err := registry.Execute(context.Background(), "date_time", json.RawMessage(`{}`))
	if err != nil || value != "now" {
		t.Fatalf("Execute() = %v, %v", value, err)
	}
	if _, err := registry.Execute(context.Background(), "weather", json.RawMessage(`{}`)); !errors.Is(err, ErrNotAvailable) {
		t.Fatalf("missing tool error = %v", err)
	}
}
