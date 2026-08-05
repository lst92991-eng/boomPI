package config

import (
	"os"
	"path/filepath"
	"runtime"
	"strings"
	"testing"
)

func TestCreateStarterWritesOneFieldAndNeverOverwrites(t *testing.T) {
	path := filepath.Join(t.TempDir(), "nested", "config.yaml")
	created, err := CreateStarter(path, "starter-secret")
	if err != nil || !created {
		t.Fatalf("CreateStarter() = %t, %v", created, err)
	}
	info, err := os.Stat(path)
	if err != nil {
		t.Fatalf("Stat() error = %v", err)
	}
	if runtime.GOOS != "windows" && info.Mode().Perm()&0o077 != 0 {
		t.Fatalf("config permissions = %o", info.Mode().Perm())
	}
	before, err := os.ReadFile(path)
	if err != nil {
		t.Fatalf("ReadFile() error = %v", err)
	}
	if !strings.Contains(string(before), "qwen_api_key: starter-secret") || strings.Contains(string(before), "device_token:") {
		t.Fatalf("starter config is not minimal: %s", before)
	}
	created, err = CreateStarter(path, "different-secret")
	if err != nil || created {
		t.Fatalf("second CreateStarter() = %t, %v", created, err)
	}
	after, _ := os.ReadFile(path)
	if string(after) != string(before) {
		t.Fatal("CreateStarter overwrote the existing file")
	}
}

func TestCreateStarterRejectsEmptyKey(t *testing.T) {
	path := filepath.Join(t.TempDir(), "config.yaml")
	if _, err := CreateStarter(path, "  "); err == nil {
		t.Fatal("CreateStarter accepted an empty key")
	}
	if _, err := os.Stat(path); !os.IsNotExist(err) {
		t.Fatal("CreateStarter left a file after invalid input")
	}
}
