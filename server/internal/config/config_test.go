package config

import (
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestLoadExampleAndEnvironmentCredential(t *testing.T) {
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
	formatted := fmt.Sprintf("%+v", cfg)
	if strings.Contains(formatted, "dashscope-secret") {
		t.Fatal("formatted configuration leaked an API key")
	}
}

func TestLoadRejectsSecretAndUnknownYAMLKeys(t *testing.T) {
	t.Setenv("DASHSCOPE_API_KEY", "environment-secret")
	t.Setenv("DASHSCOPE_WORKSPACE_ID", "test-workspace")
	for _, content := range []string{
		"api_key: do-not-store-this\n",
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

func TestLoadRejectsInvalidAndOversizedConfiguration(t *testing.T) {
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
	t.Setenv("DASHSCOPE_API_KEY", "test-secret")
	t.Setenv("DASHSCOPE_WORKSPACE_ID", "test-workspace")
	path := writeConfig(t, "listen_address: localhost\n")
	if _, err := Load(path, nil); err == nil || !strings.Contains(err.Error(), "IPv4 or IPv6") {
		t.Fatalf("hostname listen address error = %v", err)
	}
}

func TestLoadRequiresEnvironmentCredential(t *testing.T) {
	t.Setenv("DASHSCOPE_API_KEY", "")
	t.Setenv("DASHSCOPE_WORKSPACE_ID", "test-workspace")
	_, err := Load("", nil)
	if err == nil || !strings.Contains(err.Error(), "DASHSCOPE_API_KEY") {
		t.Fatalf("missing credential error = %v", err)
	}
}

func TestLoadRequiresWorkspaceID(t *testing.T) {
	t.Setenv("DASHSCOPE_API_KEY", "test-secret")
	t.Setenv("DASHSCOPE_WORKSPACE_ID", "")
	_, err := Load("", nil)
	if err == nil || !strings.Contains(err.Error(), "DASHSCOPE_WORKSPACE_ID") {
		t.Fatalf("missing workspace ID error = %v", err)
	}
}

func TestLoadPrecedenceCLIEnvironmentYAMLDefaults(t *testing.T) {
	t.Setenv("DASHSCOPE_API_KEY", "test-secret")
	t.Setenv("DASHSCOPE_WORKSPACE_ID", "test-workspace")
	t.Setenv("BOOMPI_WSS_PORT", "18002")
	t.Setenv("BOOMPI_REGION", "singapore-env")
	path := writeConfig(t, "wss_port: 18001\nregion: singapore-yaml\ndiscovery_port: 17807\n")

	cfg, err := Load(path, Overrides{"wss_port": "18003"})
	if err != nil {
		t.Fatalf("Load() error = %v", err)
	}
	if cfg.WSSPort != 18003 {
		t.Fatalf("WSSPort = %d, want CLI value 18003", cfg.WSSPort)
	}
	if cfg.Region != "singapore-env" {
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
			if err := cfg.Validate(); err == nil || !strings.Contains(err.Error(), "control characters") {
				t.Fatalf("Validate() error = %v", err)
			}
		})
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
