package identity

import (
	"crypto/tls"
	"os"
	"path/filepath"
	"testing"
)

func TestLoadOrCreateKeepsStableIdentity(t *testing.T) {
	directory := t.TempDir()
	first, err := LoadOrCreate(directory)
	if err != nil {
		t.Fatalf("LoadOrCreate() first error = %v", err)
	}
	second, err := LoadOrCreate(directory)
	if err != nil {
		t.Fatalf("LoadOrCreate() second error = %v", err)
	}
	if first.SPKISHA256 == "" || first.SPKISHA256 != second.SPKISHA256 {
		t.Fatalf("SPKI digest changed: first=%q second=%q", first.SPKISHA256, second.SPKISHA256)
	}
	if first.TLSConfig.MinVersion != tls.VersionTLS12 {
		t.Fatalf("MinVersion = %d", first.TLSConfig.MinVersion)
	}
	if len(first.TLSConfig.Certificates) != 1 {
		t.Fatalf("certificate count = %d", len(first.TLSConfig.Certificates))
	}
}

func TestLoadOrCreateRejectsRelativeDirectory(t *testing.T) {
	if _, err := LoadOrCreate("relative"); err == nil {
		t.Fatal("LoadOrCreate() unexpectedly accepted a relative directory")
	}
}

func TestLoadOrCreateRejectsPartialIdentity(t *testing.T) {
	directory := t.TempDir()
	if err := os.WriteFile(filepath.Join(directory, privateKeyFile), []byte("incomplete"), 0o600); err != nil {
		t.Fatalf("WriteFile() error = %v", err)
	}
	if _, err := LoadOrCreate(directory); err == nil {
		t.Fatal("LoadOrCreate() unexpectedly accepted a partial identity")
	}
}
