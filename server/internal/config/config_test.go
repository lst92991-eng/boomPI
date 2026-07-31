package config

import (
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

const validTestDeviceToken = "0123456789abcdef0123456789abcdef"

func TestDefaultsUseSingapore(t *testing.T) {
	cfg := Defaults()
	if cfg.Region != "singapore" {
		t.Fatalf("Region = %q, want singapore", cfg.Region)
	}
}

func TestLoadExampleAndEnvironmentCredential(t *testing.T) {
	setValidDeviceToken(t)
	t.Setenv("DASHSCOPE_API_KEY", "dashscope-secret")
	t.Setenv("DASHSCOPE_WORKSPACE_ID", "workspace-1")

	cfg, err := Load(filepath.Join("..", "..", "configs", "config.example.yaml"), nil)
	if err != nil {
		t.Fatalf("Load() error = %v", err)
	}
	if cfg.Credentials.Source() != "DASHSCOPE_API_KEY" {
		t.Fatalf("credential source = %q", cfg.Credentials.Source())
	}
	if cfg.Credentials.APIKey() != "dashscope-secret" {
		t.Fatal("preferred API key was not loaded")
	}
	if cfg.Credentials.WorkspaceID() != "workspace-1" {
		t.Fatalf("workspace ID = %q", cfg.Credentials.WorkspaceID())
	}
	if cfg.DeviceToken.Value() != validTestDeviceToken {
		t.Fatal("device token was not loaded from BOOMPI_DEVICE_TOKEN")
	}
	if cfg.ConversationMode != "intelligence" || cfg.ASRModel != "qwen3-asr-flash" ||
		cfg.ReasoningModel != "qwen3.6-flash" || cfg.ReasoningEffort != "none" ||
		cfg.SearchMode != "off" || cfg.TTSModel != "qwen3-tts-flash-realtime" {
		t.Fatalf("intelligence pipeline config = mode %q, ASR %q, reasoning %q, TTS %q",
			cfg.ConversationMode, cfg.ASRModel, cfg.ReasoningModel, cfg.TTSModel)
	}
	formatted := fmt.Sprintf("%+v", cfg)
	if strings.Contains(formatted, "dashscope-secret") || strings.Contains(formatted, validTestDeviceToken) {
		t.Fatal("formatted configuration leaked a credential")
	}
}

func TestLoadRejectsSecretAndUnknownYAMLKeys(t *testing.T) {
	setValidDeviceToken(t)
	t.Setenv("DASHSCOPE_API_KEY", "environment-secret")
	t.Setenv("DASHSCOPE_WORKSPACE_ID", "test-workspace")
	for _, content := range []string{
		"api_key: do-not-store-this\n",
		"device_token: do-not-store-this\n",
		"unexpected_option: true\n",
	} {
		path := writeConfig(t, content)
		_, err := Load(path, nil)
		if err == nil {
			t.Fatalf("Load(%q) unexpectedly succeeded", content)
		}
		if strings.Contains(err.Error(), "do-not-store-this") || strings.Contains(err.Error(), "environment-secret") {
			t.Fatalf("error leaked a secret: %v", err)
		}
	}
}

func TestLoadRejectsDeviceTokenCLIOverride(t *testing.T) {
	setValidDeviceToken(t)
	t.Setenv("DASHSCOPE_API_KEY", "test-secret")
	t.Setenv("DASHSCOPE_WORKSPACE_ID", "test-workspace")
	cliToken := "abcdef0123456789abcdef0123456789"

	_, err := Load("", Overrides{"device_token": cliToken})
	if err == nil || !strings.Contains(err.Error(), "not supported") {
		t.Fatalf("device token CLI override error = %v", err)
	}
	if strings.Contains(err.Error(), cliToken) {
		t.Fatalf("error leaked CLI device token: %v", err)
	}
}

func TestLoadRejectsInvalidAndOversizedConfiguration(t *testing.T) {
	setValidDeviceToken(t)
	t.Setenv("DASHSCOPE_API_KEY", "test-secret")
	t.Setenv("DASHSCOPE_WORKSPACE_ID", "test-workspace")
	invalid := writeConfig(t, "wss_port: 70000\n")
	if _, err := Load(invalid, nil); err == nil || !strings.Contains(err.Error(), "wss_port") {
		t.Fatalf("invalid port error = %v", err)
	}

	oversized := writeConfig(t, "#"+strings.Repeat("x", maxConfigBytes)+"\n")
	if _, err := Load(oversized, nil); err == nil || !strings.Contains(err.Error(), "exceeds") {
		t.Fatalf("oversized config error = %v", err)
	}
}

func TestLoadRejectsHostNameListenAddress(t *testing.T) {
	setValidDeviceToken(t)
	t.Setenv("DASHSCOPE_API_KEY", "test-secret")
	t.Setenv("DASHSCOPE_WORKSPACE_ID", "test-workspace")
	path := writeConfig(t, "listen_address: localhost\n")
	if _, err := Load(path, nil); err == nil || !strings.Contains(err.Error(), "IPv4 or IPv6") {
		t.Fatalf("hostname listen address error = %v", err)
	}
}

func TestLoadRequiresEnvironmentCredential(t *testing.T) {
	setValidDeviceToken(t)
	t.Setenv("DASHSCOPE_API_KEY", "")
	t.Setenv("DASHSCOPE_WORKSPACE_ID", "test-workspace")
	_, err := Load("", nil)
	if err == nil || !strings.Contains(err.Error(), "DASHSCOPE_API_KEY") {
		t.Fatalf("missing credential error = %v", err)
	}
}

func TestLoadRequiresWorkspaceID(t *testing.T) {
	setValidDeviceToken(t)
	t.Setenv("DASHSCOPE_API_KEY", "test-secret")
	t.Setenv("DASHSCOPE_WORKSPACE_ID", "")
	_, err := Load("", nil)
	if err == nil || !strings.Contains(err.Error(), "DASHSCOPE_WORKSPACE_ID") {
		t.Fatalf("missing workspace ID error = %v", err)
	}
}

func TestLoadRequiresDeviceToken(t *testing.T) {
	t.Setenv("DASHSCOPE_API_KEY", "test-secret")
	t.Setenv("DASHSCOPE_WORKSPACE_ID", "test-workspace")
	t.Setenv("BOOMPI_DEVICE_TOKEN", "")

	_, err := Load("", nil)
	if err == nil || !strings.Contains(err.Error(), "BOOMPI_DEVICE_TOKEN") {
		t.Fatalf("missing device token error = %v", err)
	}
}

func TestLoadRejectsInvalidDeviceToken(t *testing.T) {
	t.Setenv("DASHSCOPE_API_KEY", "test-secret")
	t.Setenv("DASHSCOPE_WORKSPACE_ID", "test-workspace")

	for name, token := range map[string]string{
		"too-short":  "short-device-token",
		"whitespace": validTestDeviceToken + " ",
	} {
		t.Run(name, func(t *testing.T) {
			t.Setenv("BOOMPI_DEVICE_TOKEN", token)
			_, err := Load("", nil)
			if err == nil || !strings.Contains(err.Error(), "BOOMPI_DEVICE_TOKEN") {
				t.Fatalf("invalid device token error = %v", err)
			}
			if strings.Contains(err.Error(), token) {
				t.Fatalf("error leaked device token: %v", err)
			}
		})
	}
}

func TestDeviceTokenFormattingIsRedacted(t *testing.T) {
	token := DeviceToken{value: validTestDeviceToken}
	if got := token.String(); got != "<redacted>" {
		t.Fatalf("String() = %q, want <redacted>", got)
	}
	if got := token.GoString(); got != "<redacted>" {
		t.Fatalf("GoString() = %q, want <redacted>", got)
	}
	if got := fmt.Sprintf("%v %#v", token, token); strings.Contains(got, validTestDeviceToken) {
		t.Fatalf("formatted token leaked its value: %s", got)
	}
}

func TestLoadPrecedenceCLIEnvironmentYAMLDefaults(t *testing.T) {
	setValidDeviceToken(t)
	t.Setenv("DASHSCOPE_API_KEY", "test-secret")
	t.Setenv("DASHSCOPE_WORKSPACE_ID", "test-workspace")
	t.Setenv("BOOMPI_WSS_PORT", "18002")
	t.Setenv("BOOMPI_REGION", "singapore")
	path := writeConfig(t, "wss_port: 18001\nregion: china-beijing\ndiscovery_port: 17807\n")

	cfg, err := Load(path, Overrides{"wss_port": "18003"})
	if err != nil {
		t.Fatalf("Load() error = %v", err)
	}
	if cfg.WSSPort != 18003 {
		t.Fatalf("WSSPort = %d, want CLI value 18003", cfg.WSSPort)
	}
	if cfg.Region != "singapore" {
		t.Fatalf("Region = %q, want environment value", cfg.Region)
	}
	if cfg.DiscoveryPort != 17807 {
		t.Fatalf("DiscoveryPort = %d, want YAML value 17807", cfg.DiscoveryPort)
	}
	if cfg.LogLevel != Defaults().LogLevel {
		t.Fatalf("LogLevel = %q, want default value", cfg.LogLevel)
	}
}

func TestLoadPreservesHashInsidePlainScalars(t *testing.T) {
	setValidDeviceToken(t)
	t.Setenv("DASHSCOPE_API_KEY", "test-secret")
	t.Setenv("DASHSCOPE_WORKSPACE_ID", "test-workspace")
	path := writeConfig(t, strings.Join([]string{
		"model: Qwen-C#-model",
		"system_prompt: https://example.test/docs/#voice",
		"persona: friendly # this is a comment",
		"",
	}, "\n"))

	cfg, err := Load(path, nil)
	if err != nil {
		t.Fatalf("Load() error = %v", err)
	}
	if cfg.Model != "Qwen-C#-model" {
		t.Fatalf("Model = %q, want hash preserved", cfg.Model)
	}
	if cfg.SystemPrompt != "https://example.test/docs/#voice" {
		t.Fatalf("SystemPrompt = %q, want URL fragment preserved", cfg.SystemPrompt)
	}
	if cfg.Persona != "friendly" {
		t.Fatalf("Persona = %q, want trailing comment removed", cfg.Persona)
	}
}

func TestValidateRejectsControlCharactersInModel(t *testing.T) {
	for name, model := range map[string]string{
		"nul":       "qwen\x00model",
		"tab":       "qwen\tmodel",
		"newline":   "qwen\nmodel",
		"delete":    "qwen\x7fmodel",
		"next-line": "qwen\u0085model",
	} {
		t.Run(name, func(t *testing.T) {
			cfg := Defaults()
			cfg.Model = model
			cfg.Credentials = Credentials{apiKey: "test-secret", workspaceID: "test-workspace"}
			cfg.DeviceToken = DeviceToken{value: validTestDeviceToken}
			if err := cfg.Validate(); err == nil || !strings.Contains(err.Error(), "control characters") {
				t.Fatalf("Validate() error = %v", err)
			}
		})
	}
}

func TestValidateRejectsUnsupportedRegion(t *testing.T) {
	cfg := Defaults()
	cfg.Region = "ap-southeast-1"
	cfg.Credentials = Credentials{apiKey: "test-secret", workspaceID: "test-workspace"}
	cfg.DeviceToken = DeviceToken{value: validTestDeviceToken}
	if err := cfg.Validate(); err == nil || !strings.Contains(err.Error(), "china-beijing or singapore") {
		t.Fatalf("Validate() error = %v", err)
	}
}

func setValidDeviceToken(t *testing.T) {
	t.Helper()
	t.Setenv("BOOMPI_DEVICE_TOKEN", validTestDeviceToken)
}

func writeConfig(t *testing.T, content string) string {
	t.Helper()
	path := filepath.Join(t.TempDir(), "config.yaml")
	if err := os.WriteFile(path, []byte(content), 0o600); err != nil {
		t.Fatalf("WriteFile() error = %v", err)
	}
	return path
}
