package logging

import (
	"context"
	"errors"
	"io"
	"log/slog"
	"strings"
)

func NewJSON(writer io.Writer, levelName string) (*slog.Logger, error) {
	level, err := parseLevel(levelName)
	if err != nil {
		return nil, err
	}
	base := slog.NewJSONHandler(writer, &slog.HandlerOptions{Level: level})
	return slog.New(redactingHandler{next: base}), nil
}

func parseLevel(value string) (slog.Level, error) {
	switch strings.ToLower(value) {
	case "debug":
		return slog.LevelDebug, nil
	case "info":
		return slog.LevelInfo, nil
	case "warn":
		return slog.LevelWarn, nil
	case "error":
		return slog.LevelError, nil
	default:
		return 0, errors.New("unsupported log level")
	}
}

type redactingHandler struct {
	next      slog.Handler
	redactAll bool
}

func (h redactingHandler) Enabled(ctx context.Context, level slog.Level) bool {
	return h.next.Enabled(ctx, level)
}

func (h redactingHandler) Handle(ctx context.Context, record slog.Record) error {
	// Log messages are static event names. Potentially sensitive runtime values
	// must be structured attributes so this handler can classify and redact them.
	clean := slog.NewRecord(record.Time, record.Level, record.Message, record.PC)
	record.Attrs(func(attr slog.Attr) bool {
		clean.AddAttrs(redactAttr(attr, h.redactAll))
		return true
	})
	return h.next.Handle(ctx, clean)
}

func (h redactingHandler) WithAttrs(attrs []slog.Attr) slog.Handler {
	clean := make([]slog.Attr, 0, len(attrs))
	for _, attr := range attrs {
		clean = append(clean, redactAttr(attr, h.redactAll))
	}
	return redactingHandler{next: h.next.WithAttrs(clean), redactAll: h.redactAll}
}

func (h redactingHandler) WithGroup(name string) slog.Handler {
	return redactingHandler{
		next:      h.next.WithGroup(name),
		redactAll: h.redactAll || isSensitiveKey(name),
	}
}

func redactAttr(attr slog.Attr, redactAll bool) slog.Attr {
	attr.Value = attr.Value.Resolve()
	if redactAll || isSensitiveKey(attr.Key) {
		return slog.String(attr.Key, "<redacted>")
	}
	switch attr.Value.Kind() {
	case slog.KindGroup:
		group := attr.Value.Group()
		clean := make([]slog.Attr, 0, len(group))
		for _, child := range group {
			clean = append(clean, redactAttr(child, false))
		}
		return slog.Group(attr.Key, attrsToAny(clean)...)
	case slog.KindAny:
		return slog.String(attr.Key, "<redacted>")
	}
	return attr
}

func attrsToAny(attrs []slog.Attr) []any {
	values := make([]any, len(attrs))
	for index := range attrs {
		values[index] = attrs[index]
	}
	return values
}

func isSensitiveKey(key string) bool {
	normalized := strings.ToLower(strings.ReplaceAll(key, "-", "_"))
	switch normalized {
	case "authorization", "cookie", "api_key", "token", "secret", "password", "private_key":
		return true
	}
	for _, suffix := range []string{"_api_key", "_token", "_secret", "_password", "_private_key", "_cookie"} {
		if strings.HasSuffix(normalized, suffix) {
			return true
		}
	}
	return false
}
