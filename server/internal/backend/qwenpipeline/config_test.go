package qwenpipeline

import (
	"testing"
)

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

func TestRejectsNonChinaRegion(t *testing.T) {
	for _, region := range []string{"singapore", "unknown"} {
		cfg := Config{APIKey: "test-key", Region: region}
		if err := cfg.validate(); err == nil {
			t.Fatalf("validate() accepted non-China region %q", region)
		}
	}
}
