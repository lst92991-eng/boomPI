package qwenpipeline

import (
	"testing"
	"time"
)

func TestAPIKeyOnlyUsesPublicSingaporeEndpoints(t *testing.T) {
	cfg := Config{
		APIKey: "test-key", Region: RegionSingapore,
		ASRModel: "qwen3-asr-flash", ReasoningModel: "qwen3.6-flash",
		ReasoningEffort: "none", TTSModel: "qwen3-tts-flash-realtime",
		TTSVoice: "Cherry", SearchMode: "off", Timeout: 30 * time.Second,
		QueueSize: 64, MaxTurns: 20, MaxContextTokens: 24_000,
	}
	if err := cfg.validate(); err != nil {
		t.Fatalf("validate() error = %v", err)
	}
	if got, want := cfg.compatibleBaseURL(), "https://dashscope-intl.aliyuncs.com/compatible-mode/v1"; got != want {
		t.Fatalf("compatibleBaseURL() = %q, want %q", got, want)
	}
	if got, want := cfg.asrRealtimeURL(), "wss://dashscope-intl.aliyuncs.com/api-ws/v1/realtime?model=qwen3-asr-flash-realtime"; got != want {
		t.Fatalf("asrRealtimeURL() = %q, want %q", got, want)
	}
}

func TestWorkspaceKeepsDedicatedSingaporeEndpoint(t *testing.T) {
	cfg := Config{Region: RegionSingapore, WorkspaceID: "ws-test_123"}
	if got, want := cfg.compatibleBaseURL(), "https://ws-test_123.ap-southeast-1.maas.aliyuncs.com/compatible-mode/v1"; got != want {
		t.Fatalf("compatibleBaseURL() = %q, want %q", got, want)
	}
	if got, want := cfg.asrRealtimeURL(), "wss://ws-test_123.ap-southeast-1.maas.aliyuncs.com/api-ws/v1/realtime?model=qwen3-asr-flash-realtime"; got != want {
		t.Fatalf("asrRealtimeURL() = %q, want %q", got, want)
	}
}

func TestChinaBeijingEndpoints(t *testing.T) {
	public := Config{Region: RegionChinaBeijing, TTSModel: "qwen3-tts-flash-realtime"}
	if got, want := public.compatibleBaseURL(), "https://dashscope.aliyuncs.com/compatible-mode/v1"; got != want {
		t.Fatalf("public compatibleBaseURL() = %q, want %q", got, want)
	}
	if got, want := public.asrRealtimeURL(), "wss://dashscope.aliyuncs.com/api-ws/v1/realtime?model=qwen3-asr-flash-realtime"; got != want {
		t.Fatalf("public asrRealtimeURL() = %q, want %q", got, want)
	}
	if got, want := public.ttsURL(), "wss://dashscope.aliyuncs.com/api-ws/v1/realtime?model=qwen3-tts-flash-realtime"; got != want {
		t.Fatalf("public ttsURL() = %q, want %q", got, want)
	}

	workspace := Config{Region: RegionChinaBeijing, WorkspaceID: "ws-test_123"}
	if got, want := workspace.compatibleBaseURL(), "https://ws-test_123.cn-beijing.maas.aliyuncs.com/compatible-mode/v1"; got != want {
		t.Fatalf("workspace compatibleBaseURL() = %q, want %q", got, want)
	}
	if got, want := workspace.asrRealtimeURL(), "wss://ws-test_123.cn-beijing.maas.aliyuncs.com/api-ws/v1/realtime?model=qwen3-asr-flash-realtime"; got != want {
		t.Fatalf("workspace asrRealtimeURL() = %q, want %q", got, want)
	}
}

func TestRejectsUnknownRegion(t *testing.T) {
	cfg := Config{APIKey: "test-key", Region: "unknown"}
	if err := cfg.validate(); err == nil {
		t.Fatal("validate() accepted an unknown region")
	}
}
