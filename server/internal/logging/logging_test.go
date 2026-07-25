package logging

import (
	"bytes"
	"errors"
	"log/slog"
	"net/http"
	"strings"
	"testing"
)

func TestLoggerRedactsSensitiveAttributes(t *testing.T) {
	var output bytes.Buffer
	logger, err := NewJSON(&output, "debug")
	if err != nil {
		t.Fatalf("NewJSON() error = %v", err)
	}
	logger.Info("configured",
		"api_key", "top-secret",
		slog.Group("provider",
			"access_token", "device-secret",
			"client_secret", "client-secret",
			"session_cookie", "cookie-secret",
			"model", "qwen",
		),
	)
	text := output.String()
	for _, secret := range []string{"top-secret", "device-secret", "client-secret", "cookie-secret"} {
		if strings.Contains(text, secret) {
			t.Fatalf("log leaked %q: %s", secret, text)
		}
	}
	if !strings.Contains(text, "<redacted>") || !strings.Contains(text, "qwen") {
		t.Fatalf("log did not preserve safe fields: %s", text)
	}
}

func TestLoggerRejectsUnknownLevel(t *testing.T) {
	if _, err := NewJSON(&bytes.Buffer{}, "verbose"); err == nil {
		t.Fatal("NewJSON() unexpectedly accepted an unknown level")
	}
}

func TestLoggerRedactsEverythingInsideSensitiveWithGroup(t *testing.T) {
	var output bytes.Buffer
	logger, err := NewJSON(&output, "debug")
	if err != nil {
		t.Fatalf("NewJSON() error = %v", err)
	}

	logger.WithGroup("authorization").Info("request",
		"scheme", "Bearer",
		"credential", "with-group-secret",
	)

	text := output.String()
	if strings.Contains(text, "Bearer") || strings.Contains(text, "with-group-secret") {
		t.Fatalf("sensitive group leaked a value: %s", text)
	}
	if !strings.Contains(text, "<redacted>") {
		t.Fatalf("sensitive group did not use a safe placeholder: %s", text)
	}
}

func TestLoggerDoesNotSerializeOpaqueAnyValues(t *testing.T) {
	var output bytes.Buffer
	logger, err := NewJSON(&output, "debug")
	if err != nil {
		t.Fatalf("NewJSON() error = %v", err)
	}

	logger.Error("request failed",
		slog.Any("headers", http.Header{
			"Authorization": []string{"Bearer header-secret"},
			"X-Debug":       []string{"header-debug-value"},
		}),
		slog.Any("error", errors.New("provider rejected error-secret")),
		slog.Any("metadata", map[string]any{"token": "map-secret"}),
		slog.Any("items", []string{"slice-secret"}),
		slog.Any("request", struct{ Value string }{Value: "struct-secret"}),
	)

	text := output.String()
	for _, secret := range []string{
		"header-secret",
		"header-debug-value",
		"error-secret",
		"map-secret",
		"slice-secret",
		"struct-secret",
	} {
		if strings.Contains(text, secret) {
			t.Fatalf("opaque slog.Any value leaked %q: %s", secret, text)
		}
	}
	if strings.Count(text, "<redacted>") < 5 {
		t.Fatalf("opaque values did not use safe placeholders: %s", text)
	}
}
