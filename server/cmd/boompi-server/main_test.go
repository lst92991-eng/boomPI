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
	if strings.Contains(combined, "command-secret") {
		t.Fatalf("command output leaked API key: %s", combined)
	}
	if !strings.Contains(stdout.String(), "configuration valid") {
		t.Fatalf("stdout = %s", stdout.String())
	}
}

func TestRunReportsMissingConfig(t *testing.T) {
	t.Setenv("DASHSCOPE_API_KEY", "command-secret")
	var stdout, stderr bytes.Buffer
	code := run(context.Background(), []string{"--config", filepath.Join(t.TempDir(), "missing.yaml")}, &stdout, &stderr)
	if code != 1 || !strings.Contains(stderr.String(), "configuration error") {
		t.Fatalf("run() code = %d, stderr = %s", code, stderr.String())
	}
}

func TestCLIOverrideWinsOverEnvironmentAndYAML(t *testing.T) {
	t.Setenv("DASHSCOPE_API_KEY", "command-secret")
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
