package config

import (
	"os"
	"path/filepath"
	"runtime"
	"strings"
	"testing"
)

func TestCreateStarterCreatesPrivateStableConfiguration(t *testing.T) {
	path := filepath.Join(t.TempDir(), "nested", "config.yaml")
	created, err := CreateStarter(path)
	if err != nil || !created {
		t.Fatalf("CreateStarter() = %t, %v", created, err)
	}
	info, err := os.Stat(path)
	if err != nil {
		t.Fatalf("Stat() error = %v", err)
	}
	if runtime.GOOS != "windows" && info.Mode().Perm()&0o077 != 0 {
		t.Fatalf("config permissions = %o, want no group/other access", info.Mode().Perm())
	}
	before, err := os.ReadFile(path)
	if err != nil {
		t.Fatalf("ReadFile() error = %v", err)
	}
	if !strings.Contains(string(before), "qwen_api_key: \""+APIKeyPlaceholder+"\"") ||
		!strings.Contains(string(before), "device_token:") {
		t.Fatalf("starter configuration is incomplete: %s", before)
	}

	created, err = CreateStarter(path)
	if err != nil || created {
		t.Fatalf("second CreateStarter() = %t, %v", created, err)
	}
	after, err := os.ReadFile(path)
	if err != nil {
		t.Fatalf("ReadFile() error = %v", err)
	}
	if string(after) != string(before) {
		t.Fatal("CreateStarter overwrote the existing configuration")
	}
}

func TestStarterNeedsOnlyAPIKeyOverride(t *testing.T) {
	path := filepath.Join(t.TempDir(), "config.yaml")
	if created, err := CreateStarter(path); err != nil || !created {
		t.Fatalf("CreateStarter() = %t, %v", created, err)
	}
	t.Setenv("DASHSCOPE_API_KEY", "")
	t.Setenv("DASHSCOPE_WORKSPACE_ID", "")
	t.Setenv("BOOMPI_DEVICE_TOKEN", "")
	if _, err := Load(path, nil); err == nil || !strings.Contains(err.Error(), "qwen_api_key") {
		t.Fatalf("placeholder Load() error = %v", err)
	}

	t.Setenv("DASHSCOPE_API_KEY", "environment-secret")
	cfg, err := Load(path, nil)
	if err != nil {
		t.Fatalf("API-key-only Load() error = %v", err)
	}
	if cfg.Credentials.Source() != "DASHSCOPE_API_KEY" || len(cfg.DeviceToken.Value()) < 32 {
		t.Fatalf("loaded source/token = %q/%d", cfg.Credentials.Source(), len(cfg.DeviceToken.Value()))
	}
}
