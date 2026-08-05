package main

import (
	"bytes"
	"context"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestCheckConfigDoesNotPrintSecret(t *testing.T) {
	t.Setenv("DASHSCOPE_API_KEY", "command-secret")
	t.Setenv("DASHSCOPE_WORKSPACE_ID", "test-workspace")
	configPath := filepath.Join(t.TempDir(), "config.yaml")
	if err := os.WriteFile(configPath, []byte("log_level: debug\n"), 0o600); err != nil {
		t.Fatalf("WriteFile() error = %v", err)
	}
	var stdout, stderr bytes.Buffer
	code := run(context.Background(), []string{"--config", configPath, "--check-config"}, strings.NewReader(""), &stdout, &stderr)
	if code != 0 {
		t.Fatalf("run() code = %d, stderr = %s", code, stderr.String())
	}
	combined := stdout.String() + stderr.String()
	if strings.Contains(combined, "command-secret") || strings.Contains(combined, "0123456789abcdef0123456789abcdef") {
		t.Fatalf("command output leaked a credential: %s", combined)
	}
	if !strings.Contains(stdout.String(), "configuration valid") {
		t.Fatalf("stdout = %s", stdout.String())
	}
}

func TestCheckConfigRedactsYAMLKey(t *testing.T) {
	t.Setenv("DASHSCOPE_API_KEY", "")
	t.Setenv("DASHSCOPE_WORKSPACE_ID", "")
	apiKey := "yaml-command-secret"
	configPath := filepath.Join(t.TempDir(), "config.yaml")
	contents := "qwen_api_key: \"" + apiKey + "\"\n"
	if err := os.WriteFile(configPath, []byte(contents), 0o600); err != nil {
		t.Fatalf("WriteFile() error = %v", err)
	}
	var stdout, stderr bytes.Buffer
	code := run(context.Background(), []string{"--config", configPath, "--check-config"}, strings.NewReader(""), &stdout, &stderr)
	if code != 0 {
		t.Fatalf("run() code = %d, stderr = %s", code, stderr.String())
	}
	combined := stdout.String() + stderr.String()
	if strings.Contains(combined, apiKey) {
		t.Fatalf("command output leaked a YAML credential: %s", combined)
	}
	if !strings.Contains(stdout.String(), "credential_source=config.yaml") {
		t.Fatalf("stdout = %s", stdout.String())
	}
}

func TestFirstRunPromptsSavesKeyAndContinues(t *testing.T) {
	t.Setenv("DASHSCOPE_API_KEY", "")
	configPath := filepath.Join(t.TempDir(), "config.yaml")
	var stdout, stderr bytes.Buffer
	code := run(context.Background(), []string{"--config", configPath, "--check-config"}, strings.NewReader("pasted-secret\n"), &stdout, &stderr)
	if code != 0 || stderr.Len() != 0 {
		t.Fatalf("run() code = %d, stderr = %s", code, stderr.String())
	}
	if !strings.Contains(stdout.String(), "首次运行") || !strings.Contains(stdout.String(), "正在启动") ||
		!strings.Contains(stdout.String(), "configuration valid") || strings.Contains(stdout.String(), "pasted-secret") {
		t.Fatalf("starter output = %q", stdout.String())
	}
	contents, err := os.ReadFile(configPath)
	if err != nil {
		t.Fatalf("ReadFile() error = %v", err)
	}
	if !strings.Contains(string(contents), "qwen_api_key: pasted-secret") || strings.Contains(string(contents), "device_token") {
		t.Fatalf("starter config = %s", contents)
	}
}

func TestFirstRunCanUseEnvironmentKeyWithoutPrompt(t *testing.T) {
	t.Setenv("DASHSCOPE_API_KEY", "command-secret")
	configPath := filepath.Join(t.TempDir(), "config.yaml")
	var stdout, stderr bytes.Buffer
	code := run(context.Background(), []string{"--config", configPath, "--check-config"}, strings.NewReader(""), &stdout, &stderr)
	if code != 0 {
		t.Fatalf("run() code = %d, stderr = %s", code, stderr.String())
	}
	if strings.Contains(stdout.String(), "首次运行") || strings.Contains(stdout.String(), "command-secret") {
		t.Fatalf("environment setup output = %s", stdout.String())
	}
}

func TestHelpReturnsSuccess(t *testing.T) {
	var stdout, stderr bytes.Buffer
	code := run(context.Background(), []string{"--help"}, strings.NewReader(""), &stdout, &stderr)
	if code != 0 {
		t.Fatalf("run() code = %d, stderr = %s", code, stderr.String())
	}
	if !strings.Contains(stderr.String(), "Usage of boompi-server") {
		t.Fatalf("help output = %q", stderr.String())
	}
}

func TestDefaultConfigLivesBesideExecutable(t *testing.T) {
	executable, err := os.Executable()
	if err != nil {
		t.Fatalf("os.Executable() error = %v", err)
	}
	want := filepath.Join(filepath.Dir(executable), "config.yaml")
	if got := defaultConfigPath(); got != want {
		t.Fatalf("defaultConfigPath() = %q, want %q", got, want)
	}
}
