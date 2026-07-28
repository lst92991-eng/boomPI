package qwen

import (
	"strings"
	"testing"
	"time"
)

func TestSingaporeEndpoint(t *testing.T) {
	endpoint, err := SingaporeEndpoint("ws-test_123")
	if err != nil {
		t.Fatalf("SingaporeEndpoint() error = %v", err)
	}
	if endpoint != "wss://ws-test_123.ap-southeast-1.maas.aliyuncs.com/api-ws/v1/realtime" {
		t.Fatalf("SingaporeEndpoint() = %q", endpoint)
	}
	if _, err := SingaporeEndpoint("bad/id"); err == nil {
		t.Fatal("SingaporeEndpoint() accepted an invalid workspace ID")
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
