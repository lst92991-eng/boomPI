package identity

import (
	"crypto/ecdsa"
	"crypto/elliptic"
	"crypto/rand"
	"crypto/sha256"
	"crypto/tls"
	"crypto/x509"
	"crypto/x509/pkix"
	"encoding/base64"
	"encoding/pem"
	"errors"
	"fmt"
	"math/big"
	"os"
	"path/filepath"
	"time"
)

const (
	certificateFile = "server.crt"
	privateKeyFile  = "server.key"
)

type ServerIdentity struct {
	TLSConfig  *tls.Config
	SPKISHA256 string
}

// LoadOrCreate loads the stable local server key or creates it on first start.
// The returned SPKI digest is the value stored by a paired boomPI device.
func LoadOrCreate(directory string) (ServerIdentity, error) {
	if !filepath.IsAbs(directory) {
		return ServerIdentity{}, errors.New("identity directory must be absolute")
	}
	if err := os.MkdirAll(directory, 0o700); err != nil {
		return ServerIdentity{}, fmt.Errorf("create identity directory: %w", err)
	}

	certificatePath := filepath.Join(directory, certificateFile)
	privateKeyPath := filepath.Join(directory, privateKeyFile)
	certificateExists, err := pathExists(certificatePath)
	if err != nil {
		return ServerIdentity{}, fmt.Errorf("inspect server certificate: %w", err)
	}
	privateKeyExists, err := pathExists(privateKeyPath)
	if err != nil {
		return ServerIdentity{}, fmt.Errorf("inspect server private key: %w", err)
	}
	if certificateExists != privateKeyExists {
		if certificateExists {
			// A certificate without its key cannot sign anything; fail closed so
			// the operator notices instead of silently minting a new identity.
			return ServerIdentity{}, errors.New("server certificate exists without its private key")
		}
		// Orphaned private key from an interrupted first-start: without the
		// certificate no device can hold a matching SPKI pin, so it is safe
		// to remove it and regenerate instead of refusing to boot forever.
		if err := os.Remove(privateKeyPath); err != nil {
			return ServerIdentity{}, fmt.Errorf("remove orphaned server private key: %w", err)
		}
	}
	if !certificateExists {
		if err := createIdentity(certificatePath, privateKeyPath); err != nil {
			return ServerIdentity{}, err
		}
	}

	pair, err := tls.LoadX509KeyPair(certificatePath, privateKeyPath)
	if err != nil {
		return ServerIdentity{}, fmt.Errorf("load server identity: %w", err)
	}
	if len(pair.Certificate) == 0 {
		return ServerIdentity{}, errors.New("server certificate chain is empty")
	}
	leaf, err := x509.ParseCertificate(pair.Certificate[0])
	if err != nil {
		return ServerIdentity{}, fmt.Errorf("parse server certificate: %w", err)
	}
	digest := sha256.Sum256(leaf.RawSubjectPublicKeyInfo)
	return ServerIdentity{
		TLSConfig: &tls.Config{
			MinVersion:   tls.VersionTLS12,
			Certificates: []tls.Certificate{pair},
			NextProtos:   []string{"http/1.1"},
		},
		SPKISHA256: base64.StdEncoding.EncodeToString(digest[:]),
	}, nil
}

func createIdentity(certificatePath, privateKeyPath string) error {
	privateKey, err := ecdsa.GenerateKey(elliptic.P256(), rand.Reader)
	if err != nil {
		return fmt.Errorf("generate server private key: %w", err)
	}
	serialLimit := new(big.Int).Lsh(big.NewInt(1), 128)
	serialNumber, err := rand.Int(rand.Reader, serialLimit)
	if err != nil {
		return fmt.Errorf("generate certificate serial: %w", err)
	}
	now := time.Now()
	template := x509.Certificate{
		SerialNumber: serialNumber,
		Subject:      pkix.Name{CommonName: "boomPI local server"},
		NotBefore:    now.Add(-5 * time.Minute),
		NotAfter:     now.AddDate(10, 0, 0),
		KeyUsage:     x509.KeyUsageDigitalSignature,
		ExtKeyUsage:  []x509.ExtKeyUsage{x509.ExtKeyUsageServerAuth},
		DNSNames:     []string{"boompi.local", "localhost"},
	}
	der, err := x509.CreateCertificate(rand.Reader, &template, &template, &privateKey.PublicKey, privateKey)
	if err != nil {
		return fmt.Errorf("create server certificate: %w", err)
	}
	privateKeyDER, err := x509.MarshalPKCS8PrivateKey(privateKey)
	if err != nil {
		return fmt.Errorf("encode server private key: %w", err)
	}
	keyPEM := pem.EncodeToMemory(&pem.Block{Type: "PRIVATE KEY", Bytes: privateKeyDER})
	certificatePEM := pem.EncodeToMemory(&pem.Block{Type: "CERTIFICATE", Bytes: der})
	if err := writeNewFile(privateKeyPath, keyPEM, 0o600); err != nil {
		return fmt.Errorf("write server private key: %w", err)
	}
	if err := writeNewFile(certificatePath, certificatePEM, 0o600); err != nil {
		_ = os.Remove(privateKeyPath)
		return fmt.Errorf("write server certificate: %w", err)
	}
	return nil
}

func writeNewFile(path string, data []byte, permission os.FileMode) error {
	file, err := os.OpenFile(path, os.O_WRONLY|os.O_CREATE|os.O_EXCL, permission)
	if err != nil {
		return err
	}
	if _, err := file.Write(data); err != nil {
		_ = file.Close()
		_ = os.Remove(path)
		return err
	}
	if err := file.Sync(); err != nil {
		_ = file.Close()
		_ = os.Remove(path)
		return err
	}
	if err := file.Close(); err != nil {
		_ = os.Remove(path)
		return err
	}
	return nil
}

func pathExists(path string) (bool, error) {
	_, err := os.Stat(path)
	if err == nil {
		return true, nil
	}
	if os.IsNotExist(err) {
		return false, nil
	}
	return false, err
}
