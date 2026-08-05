package config

import (
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

func TestDefaultsKeepTeachingAudioParameters(t *testing.T) {
	cfg := Defaults()
	if cfg.DiscoveryPort != 17807 || cfg.ASRModel != "qwen3-asr-flash" ||
		cfg.ReasoningModel != "qwen3.6-flash" || cfg.ReasoningEffort != "none" ||
		cfg.TTSModel != "qwen3-tts-flash-realtime" || cfg.TTSVoice != "Cherry" {
		t.Fatalf("unexpected teaching defaults: %+v", cfg)
	}
	if cfg.HeartbeatInterval != 10*time.Second || cfg.ConnectionTimeout != 30*time.Second ||
		cfg.FirstResponseTimeout != 30*time.Second {
		t.Fatalf("unexpected timeouts: %+v", cfg)
	}
}

func TestLoadNeedsOnlyYAMLAPIKey(t *testing.T) {
	t.Setenv("DASHSCOPE_API_KEY", "")
	t.Setenv("DASHSCOPE_WORKSPACE_ID", "")
	path := writeConfig(t, "qwen_api_key: yaml-secret\n")
	cfg, err := Load(path)
	if err != nil {
		t.Fatalf("Load() error = %v", err)
	}
	if cfg.Credentials.APIKey() != "yaml-secret" || cfg.Credentials.Source() != "config.yaml" {
		t.Fatalf("credential source = %q", cfg.Credentials.Source())
	}
	if cfg.DeviceToken.Value() != defaultDeviceToken {
		t.Fatal("one-field config did not use the teaching device token")
	}
}

func TestEnvironmentOverridesOnlyProviderCredentials(t *testing.T) {
	t.Setenv("DASHSCOPE_API_KEY", "environment-secret")
	t.Setenv("DASHSCOPE_WORKSPACE_ID", "workspace-1")
	path := writeConfig(t, "qwen_api_key: yaml-secret\nwss_port: 18006\n")
	cfg, err := Load(path)
	if err != nil {
		t.Fatalf("Load() error = %v", err)
	}
	if cfg.Credentials.Source() != "DASHSCOPE_API_KEY" || cfg.Credentials.WorkspaceID() != "workspace-1" ||
		cfg.DeviceToken.Value() != defaultDeviceToken || cfg.WSSPort != 18006 {
		t.Fatalf("environment/YAML precedence is wrong: %+v", cfg)
	}
	formatted := fmt.Sprintf("%+v", cfg)
	if strings.Contains(formatted, "environment-secret") || strings.Contains(formatted, defaultDeviceToken) {
		t.Fatal("formatted configuration leaked a secret")
	}
}

func TestLoadExampleUsesCurrentPipeline(t *testing.T) {
	t.Setenv("DASHSCOPE_API_KEY", "test-secret")
	t.Setenv("DASHSCOPE_WORKSPACE_ID", "")
	cfg, err := Load(filepath.Join("..", "..", "configs", "config.example.yaml"))
	if err != nil {
		t.Fatalf("Load(example) error = %v", err)
	}
	if cfg.ReasoningEffort != "none" || cfg.SearchMode != "off" || cfg.TTSVoice != "Cherry" {
		t.Fatalf("example changed runtime parameters: %+v", cfg)
	}
}

func TestStrictYAMLRejectsUnknownDuplicateAndMultipleDocuments(t *testing.T) {
	t.Setenv("DASHSCOPE_API_KEY", "test-secret")
	for name, contents := range map[string]string{
		"unknown":      "provider: qwen\n",
		"discovery":    "discovery_port: 18007\n",
		"device-token": "device_token: class-secret-should-not-be-configured\n",
		"duplicate":    "wss_port: 17806\nwss_port: 18006\n",
		"documents":    "log_level: info\n---\nlog_level: debug\n",
	} {
		t.Run(name, func(t *testing.T) {
			if _, err := Load(writeConfig(t, contents)); err == nil {
				t.Fatal("Load() accepted invalid YAML")
			}
		})
	}
}

func TestLoadRejectsOversizedConfigAndIPv6(t *testing.T) {
	t.Setenv("DASHSCOPE_API_KEY", "test-secret")
	oversized := writeConfig(t, "#"+strings.Repeat("x", maxConfigBytes)+"\n")
	if _, err := Load(oversized); err == nil || !strings.Contains(err.Error(), "exceeds") {
		t.Fatalf("oversized config error = %v", err)
	}
	ipv6 := writeConfig(t, "listen_address: '::1'\n")
	if _, err := Load(ipv6); err == nil || !strings.Contains(err.Error(), "IPv4") {
		t.Fatalf("IPv6 config error = %v", err)
	}
}

func TestAdvancedParametersRemainConfigurable(t *testing.T) {
	t.Setenv("DASHSCOPE_API_KEY", "test-secret")
	path := writeConfig(t, strings.Join([]string{
		"reasoning_model: qwen-custom",
		"reasoning_effort: low",
		"tts_voice: Serena",
		"first_response_timeout: 45s",
		"session_idle_timeout: 1h",
		"max_turns: 40",
		"max_context_tokens: 32000",
	}, "\n")+"\n")
	cfg, err := Load(path)
	if err != nil {
		t.Fatalf("Load() error = %v", err)
	}
	if cfg.ReasoningModel != "qwen-custom" || cfg.ReasoningEffort != "low" ||
		cfg.TTSVoice != "Serena" || cfg.FirstResponseTimeout != 45*time.Second ||
		cfg.SessionIdleTimeout != time.Hour || cfg.MaxTurns != 40 || cfg.MaxContextTokens != 32_000 {
		t.Fatalf("advanced configuration was not applied: %+v", cfg)
	}
}

func TestValidateKeepsHeartbeatAndTokenBounds(t *testing.T) {
	valid := Defaults()
	valid.Credentials = Credentials{apiKey: "test-secret"}
	if err := valid.Validate(); err != nil {
		t.Fatalf("default validation error = %v", err)
	}

	badHeartbeat := valid
	badHeartbeat.HeartbeatInterval = 11 * time.Second
	if err := badHeartbeat.Validate(); err == nil {
		t.Fatal("Validate() accepted heartbeat beyond client timeout contract")
	}
	badToken := valid
	badToken.DeviceToken = DeviceToken{value: "too-short"}
	if err := badToken.Validate(); err == nil {
		t.Fatal("Validate() accepted a short device token")
	}
}

func writeConfig(t *testing.T, content string) string {
	t.Helper()
	path := filepath.Join(t.TempDir(), "config.yaml")
	if err := os.WriteFile(path, []byte(content), 0o600); err != nil {
		t.Fatalf("WriteFile() error = %v", err)
	}
	return path
}
