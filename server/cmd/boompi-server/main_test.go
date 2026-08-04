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
	t.Setenv("BOOMPI_DEVICE_TOKEN", "0123456789abcdef0123456789abcdef")
	configPath := filepath.Join(t.TempDir(), "config.yaml")
	if err := os.WriteFile(configPath, []byte("log_level: debug\n"), 0o600); err != nil {
		t.Fatalf("WriteFile() error = %v", err)
	}
	var stdout, stderr bytes.Buffer
	code := run(context.Background(), []string{"--config", configPath, "--check-config"}, &stdout, &stderr)
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

func TestCheckConfigRedactsYAMLSecrets(t *testing.T) {
	t.Setenv("DASHSCOPE_API_KEY", "")
	t.Setenv("DASHSCOPE_WORKSPACE_ID", "")
	t.Setenv("BOOMPI_DEVICE_TOKEN", "")
	apiKey := "yaml-command-secret"
	deviceToken := "abcdef0123456789abcdef0123456789"
	configPath := filepath.Join(t.TempDir(), "config.yaml")
	contents := "qwen_api_key: \"" + apiKey + "\"\ndevice_token: \"" + deviceToken + "\"\n"
	if err := os.WriteFile(configPath, []byte(contents), 0o600); err != nil {
		t.Fatalf("WriteFile() error = %v", err)
	}
	var stdout, stderr bytes.Buffer
	code := run(context.Background(), []string{"--config", configPath, "--check-config"}, &stdout, &stderr)
	if code != 0 {
		t.Fatalf("run() code = %d, stderr = %s", code, stderr.String())
	}
	combined := stdout.String() + stderr.String()
	if strings.Contains(combined, apiKey) || strings.Contains(combined, deviceToken) {
		t.Fatalf("command output leaked a YAML credential: %s", combined)
	}
	if !strings.Contains(stdout.String(), "credential_source=config.yaml") {
		t.Fatalf("stdout = %s", stdout.String())
	}
}

func TestRunCreatesMissingStarterConfigAndExits(t *testing.T) {
	t.Setenv("DASHSCOPE_API_KEY", "command-secret")
	configPath := filepath.Join(t.TempDir(), "config.yaml")
	var stdout, stderr bytes.Buffer
	code := run(context.Background(), []string{"--config", configPath}, &stdout, &stderr)
	if code != 0 || stderr.Len() != 0 {
		t.Fatalf("run() code = %d, stderr = %s", code, stderr.String())
	}
	if !strings.Contains(stdout.String(), "Created starter configuration") ||
		!strings.Contains(stdout.String(), "Edit qwen_api_key") ||
		strings.Contains(stdout.String(), "command-secret") {
		t.Fatalf("starter output = %q", stdout.String())
	}
	contents, err := os.ReadFile(configPath)
	if err != nil {
		t.Fatalf("ReadFile() error = %v", err)
	}
	if !strings.Contains(string(contents), "qwen_api_key") || !strings.Contains(string(contents), "device_token") {
		t.Fatalf("starter config = %s", contents)
	}
}

func TestCLIOverrideWinsOverEnvironmentAndYAML(t *testing.T) {
	t.Setenv("DASHSCOPE_API_KEY", "command-secret")
	t.Setenv("DASHSCOPE_WORKSPACE_ID", "test-workspace")
	t.Setenv("BOOMPI_DEVICE_TOKEN", "0123456789abcdef0123456789abcdef")
	t.Setenv("BOOMPI_WSS_PORT", "70000")
	configPath := filepath.Join(t.TempDir(), "config.yaml")
	if err := os.WriteFile(configPath, []byte("wss_port: 18001\n"), 0o600); err != nil {
		t.Fatalf("WriteFile() error = %v", err)
	}
	var stdout, stderr bytes.Buffer
	code := run(context.Background(), []string{"--config", configPath, "--wss-port", "18003", "--check-config"}, &stdout, &stderr)
	if code != 0 {
		t.Fatalf("run() code = %d, stderr = %s", code, stderr.String())
	}
	if !strings.Contains(stdout.String(), "wss_port=18003") {
		t.Fatalf("CLI override not reflected in output: %s", stdout.String())
	}
}

func TestHelpReturnsSuccess(t *testing.T) {
	var stdout, stderr bytes.Buffer
	code := run(context.Background(), []string{"--help"}, &stdout, &stderr)
	if code != 0 {
		t.Fatalf("run() code = %d, stderr = %s", code, stderr.String())
	}
	if !strings.Contains(stderr.String(), "Usage of boompi-server") {
		t.Fatalf("help output = %q", stderr.String())
	}
}
