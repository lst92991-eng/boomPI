package qwen

import (
	"context"
	"os"
	"testing"
	"time"

	"github.com/lst92991-eng/boomPI/server/internal/backend"
)

func TestLiveOpenSession(t *testing.T) {
	if os.Getenv("BOOMPI_QWEN_LIVE_TEST") != "1" {
		t.Skip("set BOOMPI_QWEN_LIVE_TEST=1 for the explicit paid provider smoke")
	}
	apiKey := os.Getenv("DASHSCOPE_API_KEY")
	workspaceID := os.Getenv("DASHSCOPE_WORKSPACE_ID")
	if apiKey == "" || workspaceID == "" {
		t.Fatal("DASHSCOPE_API_KEY and DASHSCOPE_WORKSPACE_ID are required")
	}
	model := os.Getenv("BOOMPI_QWEN_LIVE_MODEL")
	if model == "" {
		model = "qwen3.5-omni-plus-realtime"
	}
	voice := os.Getenv("BOOMPI_QWEN_LIVE_VOICE")
	if voice == "" {
		voice = "Ethan"
	}

	provider, err := New(Config{
		APIKey: apiKey, WorkspaceID: workspaceID, Region: RegionSingapore,
		Model: model, Voice: voice,
		Timeout: 30 * time.Second, QueueSize: 8,
	})
	if err != nil {
		t.Fatalf("New() error = %v", err)
	}
	ctx, cancel := context.WithTimeout(context.Background(), 35*time.Second)
	defer cancel()
	session, err := provider.Open(ctx, backend.SessionConfig{
		DeviceID:     "00000000-0000-4000-8000-000000000001",
		SystemPrompt: "Reply briefly in Simplified Chinese.",
		Persona:      "Natural and concise.",
	})
	if err != nil {
		t.Fatalf("Open() error = %v", err)
	}
	if err := session.Close(); err != nil {
		t.Fatalf("Close() error = %v", err)
	}
}
