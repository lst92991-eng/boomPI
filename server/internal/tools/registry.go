package tools

import (
	"context"
	"encoding/json"
	"errors"
	"sort"
	"sync"
)

var ErrNotAvailable = errors.New("tool is not available")

var allowed = map[string]struct{}{
	"date_time":          {},
	"weather":            {},
	"volume":             {},
	"brightness":         {},
	"network_status":     {},
	"device_status":      {},
	"clear_conversation": {},
	"timer":              {},
	"alarm":              {},
}

type Handler func(ctx context.Context, arguments json.RawMessage) (any, error)

type Registry struct {
	mu       sync.RWMutex
	handlers map[string]Handler
}

func NewRegistry() *Registry {
	return &Registry{handlers: make(map[string]Handler)}
}

func (r *Registry) Register(name string, handler Handler) error {
	if _, ok := allowed[name]; !ok {
		return errors.New("tool is not in the production allowlist")
	}
	if handler == nil {
		return errors.New("tool handler is required")
	}
	r.mu.Lock()
	defer r.mu.Unlock()
	if _, exists := r.handlers[name]; exists {
		return errors.New("tool is already registered")
	}
	r.handlers[name] = handler
	return nil
}

func (r *Registry) Execute(ctx context.Context, name string, arguments json.RawMessage) (any, error) {
	r.mu.RLock()
	handler, exists := r.handlers[name]
	r.mu.RUnlock()
	if !exists {
		return nil, ErrNotAvailable
	}
	return handler(ctx, arguments)
}

func AllowedNames() []string {
	names := make([]string, 0, len(allowed))
	for name := range allowed {
		names = append(names, name)
	}
	sort.Strings(names)
	return names
}
