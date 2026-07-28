package qwen

import (
	"strings"
	"testing"
	"time"
)

func TestRegionalEndpoint(t *testing.T) {
	tests := map[string]string{
		RegionChinaBeijing: "wss://ws-test_123.cn-beijing.maas.aliyuncs.com/api-ws/v1/realtime",
		RegionSingapore:    "wss://ws-test_123.ap-southeast-1.maas.aliyuncs.com/api-ws/v1/realtime",
	}
	for region, want := range tests {
		endpoint, err := RegionalEndpoint(region, "ws-test_123")
		if err != nil {
			t.Fatalf("RegionalEndpoint(%q) error = %v", region, err)
		}
		if endpoint != want {
			t.Fatalf("RegionalEndpoint(%q) = %q, want %q", region, endpoint, want)
		}
	}
	if _, err := RegionalEndpoint(RegionSingapore, "bad/id"); err == nil {
		t.Fatal("RegionalEndpoint() accepted an invalid workspace ID")
	}
	if _, err := RegionalEndpoint("unknown", "ws-test_123"); err == nil {
		t.Fatal("RegionalEndpoint() accepted an unsupported region")
	}
}

func TestConfigValidateRejectsInsecureRemoteEndpoint(t *testing.T) {
	config := validConfig("ws://example.com/api-ws/v1/realtime")
	if err := config.Validate(); err == nil || !strings.Contains(err.Error(), "loopback") {
		t.Fatalf("Validate() error = %v", err)
	}
}

func validConfig(endpoint string) Config {
	return Config{
		APIKey:    "test-api-key",
		Endpoint:  endpoint,
		Model:     "qwen3.5-omni-plus-realtime",
		Voice:     "Tina",
		Timeout:   2 * time.Second,
		QueueSize: 8,
	}
}
