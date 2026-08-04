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

func TestLoadOrCreateSelfHealsOrphanedKey(t *testing.T) {
	// 首次启动在写完私钥、写证书前中断：没有证书就不可能有设备持有匹配
	// SPKI pin，必须删除孤儿私钥并重建，而不是永久拒绝启动。
	directory := t.TempDir()
	orphan := filepath.Join(directory, privateKeyFile)
	if err := os.WriteFile(orphan, []byte("incomplete"), 0o600); err != nil {
		t.Fatalf("WriteFile() error = %v", err)
	}
	identity, err := LoadOrCreate(directory)
	if err != nil {
		t.Fatalf("LoadOrCreate() error = %v", err)
	}
	if identity.SPKISHA256 == "" {
		t.Fatal("SPKI digest empty after self-heal")
	}
	data, err := os.ReadFile(orphan)
	if err != nil {
		t.Fatalf("ReadFile() error = %v", err)
	}
	if string(data) == "incomplete" {
		t.Fatal("orphaned private key was not replaced")
	}
}

func TestLoadOrCreateFailsClosedOnOrphanedCertificate(t *testing.T) {
	// 证书在而私钥丢失：已配对设备的 pin 已失效，必须报错让运维介入，
	// 不得静默重铸新身份。
	directory := t.TempDir()
	if err := os.WriteFile(filepath.Join(directory, certificateFile), []byte("incomplete"), 0o600); err != nil {
		t.Fatalf("WriteFile() error = %v", err)
	}
	if _, err := LoadOrCreate(directory); err == nil {
		t.Fatal("LoadOrCreate() unexpectedly accepted a certificate without its key")
	}
}
