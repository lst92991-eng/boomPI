package app

import (
	"context"
	"crypto/ecdsa"
	"crypto/elliptic"
	"crypto/rand"
	"crypto/x509"
	"crypto/x509/pkix"
	"encoding/pem"
	"fmt"
	"io"
	"log/slog"
	"math/big"
	"net"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
	"testing"
	"time"

	"github.com/lst92991-eng/boomPI/server/internal/config"
)

func TestCppWSSHappyPath(t *testing.T) {
	smokeExecutable := strings.TrimSpace(os.Getenv("BOOMPI_CPP_WSS_SMOKE"))
	if smokeExecutable == "" {
		t.Skip("set BOOMPI_CPP_WSS_SMOKE to the C++ smoke executable to opt in")
	}
	if info, err := os.Stat(smokeExecutable); err != nil {
		t.Fatalf("stat C++ WSS smoke executable: %v", err)
	} else if info.IsDir() {
		t.Fatal("BOOMPI_CPP_WSS_SMOKE names a directory")
	}

	// Config.Load requires provider credentials, but newWithBackend below injects
	// the deterministic in-process fake and therefore never opens Qwen.
	t.Setenv("DASHSCOPE_API_KEY", "offline-test-key")
	t.Setenv("DASHSCOPE_WORKSPACE_ID", "offline-test-workspace")
	t.Setenv("BOOMPI_DEVICE_TOKEN", testDeviceToken)
	cfg, err := config.Load("", nil)
	if err != nil {
		t.Fatalf("config.Load() error = %v", err)
	}
	cfg.ListenAddress = "127.0.0.1"
	cfg.WSSPort = freePort(t)
	cfg.DiscoveryPort = freeUDPPort(t)
	provider := newRoundTripBackend()
	logger := slog.New(slog.NewTextHandler(io.Discard, nil))
	identityDirectory := t.TempDir()
	writeFutureServerIdentity(t, identityDirectory)
	application, err := newWithBackend(cfg, logger, identityDirectory, provider)
	if err != nil {
		t.Fatalf("newWithBackend() error = %v", err)
	}

	serverCtx, stopServer := context.WithCancel(context.Background())
	serverDone := make(chan error, 1)
	go func() { serverDone <- application.Run(serverCtx) }()
	defer func() {
		stopServer()
		select {
		case runErr := <-serverDone:
			if runErr != nil {
				t.Errorf("App.Run() error = %v", runErr)
			}
		case <-time.After(2 * time.Second):
			t.Error("server did not stop")
		}
	}()

	address := net.JoinHostPort(cfg.ListenAddress, strconv.Itoa(cfg.WSSPort))
	if err := waitForTCPListener(address, 2*time.Second); err != nil {
		t.Fatalf("wait for WSS listener: %v", err)
	}

	wrongPin := application.spkiPin
	if wrongPin[0] == 'A' {
		wrongPin = "B" + wrongPin[1:]
	} else {
		wrongPin = "A" + wrongPin[1:]
	}
	wrongPinCtx, cancelWrongPin := context.WithTimeout(context.Background(), 5*time.Second)
	wrongPinCommand := exec.CommandContext(wrongPinCtx, smokeExecutable,
		cfg.ListenAddress, strconv.Itoa(cfg.WSSPort), wrongPin)
	wrongPinCommand.Env = smokeChildEnvironment(testDeviceToken)
	wrongPinOutput, wrongPinErr := wrongPinCommand.CombinedOutput()
	wrongPinContextErr := wrongPinCtx.Err()
	cancelWrongPin()
	if wrongPinContextErr != nil {
		t.Fatalf("wrong-pin C++ WSS smoke timed out: %v", wrongPinContextErr)
	}
	if wrongPinErr == nil {
		t.Fatalf("wrong-pin C++ WSS smoke unexpectedly succeeded:\n%s", wrongPinOutput)
	}
	if strings.Contains(string(wrongPinOutput), testDeviceToken) {
		t.Fatal("wrong-pin failure output leaked the device token")
	}
	if got := provider.openCount.Load(); got != 0 {
		t.Fatalf("provider Open calls after wrong pin = %d, want 0", got)
	}

	commandCtx, cancelCommand := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancelCommand()
	command := exec.CommandContext(commandCtx, smokeExecutable,
		cfg.ListenAddress, strconv.Itoa(cfg.WSSPort), application.spkiPin)
	command.Env = smokeChildEnvironment(testDeviceToken)
	output, err := command.CombinedOutput()
	if commandCtx.Err() != nil {
		t.Fatalf("C++ WSS smoke timed out: %v", commandCtx.Err())
	}
	if err != nil {
		t.Fatalf("C++ WSS smoke failed: %v\n%s", err, output)
	}
	if !strings.Contains(string(output), "WSS_SMOKE_OK") {
		t.Fatalf("C++ WSS smoke did not report success:\n%s", output)
	}

	provider.session.mu.Lock()
	recorded := append([]byte(nil), provider.session.audio...)
	provider.session.mu.Unlock()
	if len(recorded) != inputFrameBytes || recorded[0] != 7 || recorded[1] != 8 {
		t.Fatalf("provider input length=%d prefix=%v, want one 640-byte sentinel frame",
			len(recorded), recorded[:minInt(len(recorded), 2)])
	}
	if got := provider.openCount.Load(); got != 1 {
		t.Fatalf("provider Open calls = %d, want 1", got)
	}
}

func TestCppManualSingleTurnHappyPath(t *testing.T) {
	clientExecutable := strings.TrimSpace(os.Getenv("BOOMPI_CPP_MANUAL_SINGLE_TURN"))
	if clientExecutable == "" {
		t.Skip("set BOOMPI_CPP_MANUAL_SINGLE_TURN to the boompi-client executable to opt in")
	}
	if info, err := os.Stat(clientExecutable); err != nil {
		t.Fatalf("stat C++ manual single-turn executable: %v", err)
	} else if info.IsDir() {
		t.Fatal("BOOMPI_CPP_MANUAL_SINGLE_TURN names a directory")
	}

	// Config.Load requires provider credentials, but this test injects the
	// deterministic in-process fake and never constructs the Qwen backend.
	t.Setenv("DASHSCOPE_API_KEY", "offline-test-key")
	t.Setenv("DASHSCOPE_WORKSPACE_ID", "offline-test-workspace")
	t.Setenv("BOOMPI_DEVICE_TOKEN", testDeviceToken)
	cfg, err := config.Load("", nil)
	if err != nil {
		t.Fatalf("config.Load() error = %v", err)
	}
	cfg.ListenAddress = "127.0.0.1"
	cfg.WSSPort = freePort(t)
	cfg.DiscoveryPort = freeUDPPort(t)
	provider := newRoundTripBackend()
	logger := slog.New(slog.NewTextHandler(io.Discard, nil))
	identityDirectory := t.TempDir()
	writeFutureServerIdentity(t, identityDirectory)
	application, err := newWithBackend(cfg, logger, identityDirectory, provider)
	if err != nil {
		t.Fatalf("newWithBackend() error = %v", err)
	}

	serverCtx, stopServer := context.WithCancel(context.Background())
	serverDone := make(chan error, 1)
	go func() { serverDone <- application.Run(serverCtx) }()
	defer func() {
		stopServer()
		select {
		case runErr := <-serverDone:
			if runErr != nil {
				t.Errorf("App.Run() error = %v", runErr)
			}
		case <-time.After(2 * time.Second):
			t.Error("server did not stop")
		}
	}()

	address := net.JoinHostPort(cfg.ListenAddress, strconv.Itoa(cfg.WSSPort))
	if err := waitForTCPListener(address, 2*time.Second); err != nil {
		t.Fatalf("wait for WSS listener: %v", err)
	}

	commandCtx, cancelCommand := context.WithTimeout(context.Background(), 20*time.Second)
	defer cancelCommand()
	command := exec.CommandContext(commandCtx, clientExecutable, "--manual-single-turn")
	command.Env = manualSingleTurnChildEnvironment(cfg, application.spkiPin)
	output, err := command.CombinedOutput()
	if commandCtx.Err() != nil {
		t.Fatalf("C++ manual single turn timed out: %v", commandCtx.Err())
	}
	if err != nil {
		t.Fatalf("C++ manual single turn failed: %v\n%s", err, output)
	}
	outputText := string(output)
	if !strings.Contains(outputText, "manual single turn completed") {
		t.Fatalf("C++ manual single turn did not report success:\n%s", output)
	}
	for _, forbidden := range []string{testDeviceToken, testDeviceID, "response-1"} {
		if strings.Contains(outputText, forbidden) {
			t.Fatalf("C++ manual single-turn output leaked sensitive protocol content")
		}
	}

	provider.session.mu.Lock()
	recordedBytes := len(provider.session.audio)
	provider.session.mu.Unlock()
	const expectedRecordedBytes = 3 * 16_000 * 2
	if recordedBytes != expectedRecordedBytes {
		t.Fatalf("provider input length=%d, want %d for 3 seconds", recordedBytes, expectedRecordedBytes)
	}
	if got := provider.session.commits.Load(); got != 1 {
		t.Fatalf("provider commits = %d, want 1", got)
	}
	if got := provider.openCount.Load(); got != 1 {
		t.Fatalf("provider Open calls = %d, want 1", got)
	}
}

func TestCppWSSExternalHILServer(t *testing.T) {
	if os.Getenv("BOOMPI_EXTERNAL_WSS_HIL") != "1" {
		t.Skip("set BOOMPI_EXTERNAL_WSS_HIL=1 to serve one external board smoke")
	}
	t.Setenv("DASHSCOPE_API_KEY", "offline-test-key")
	t.Setenv("DASHSCOPE_WORKSPACE_ID", "offline-test-workspace")
	t.Setenv("BOOMPI_DEVICE_TOKEN", testDeviceToken)
	cfg, err := config.Load("", nil)
	if err != nil {
		t.Fatalf("config.Load() error = %v", err)
	}
	cfg.ListenAddress = "0.0.0.0"
	cfg.WSSPort = freePort(t)
	cfg.DiscoveryPort = freeUDPPort(t)
	provider := newRoundTripBackend()
	logger := slog.New(slog.NewTextHandler(io.Discard, nil))
	identityDirectory := t.TempDir()
	writeFutureServerIdentity(t, identityDirectory)
	application, err := newWithBackend(cfg, logger, identityDirectory, provider)
	if err != nil {
		t.Fatalf("newWithBackend() error = %v", err)
	}

	serverCtx, stopServer := context.WithCancel(context.Background())
	serverDone := make(chan error, 1)
	go func() { serverDone <- application.Run(serverCtx) }()
	defer stopServer()
	loopbackAddress := net.JoinHostPort("127.0.0.1", strconv.Itoa(cfg.WSSPort))
	if err := waitForTCPListener(loopbackAddress, 2*time.Second); err != nil {
		t.Fatalf("wait for external WSS listener: %v", err)
	}
	fmt.Printf("BOOMPI_EXTERNAL_WSS_READY port=%d spki=%s\n",
		cfg.WSSPort, application.spkiPin)

	deadline := time.Now().Add(30 * time.Second)
	for {
		provider.session.mu.Lock()
		recordedBytes := len(provider.session.audio)
		provider.session.mu.Unlock()
		commits := provider.session.commits.Load()
		if recordedBytes == inputFrameBytes && commits == 1 {
			break
		}
		if recordedBytes > inputFrameBytes {
			t.Fatalf("external board sent %d bytes, want %d", recordedBytes, inputFrameBytes)
		}
		if commits > 1 {
			t.Fatalf("external board committed %d turns, want 1", commits)
		}
		select {
		case runErr := <-serverDone:
			t.Fatalf("external WSS server stopped early: %v", runErr)
		default:
		}
		if time.Now().After(deadline) {
			t.Fatal("external board WSS smoke timed out")
		}
		time.Sleep(10 * time.Millisecond)
	}

	select {
	case <-provider.session.closed:
	case <-time.After(2 * time.Second):
		t.Fatal("external board did not close the committed session")
	}
	stopServer()
	select {
	case runErr := <-serverDone:
		if runErr != nil {
			t.Fatalf("App.Run() error = %v", runErr)
		}
	case <-time.After(2 * time.Second):
		t.Fatal("external WSS server did not stop")
	}
	if got := provider.openCount.Load(); got != 1 {
		t.Fatalf("provider Open calls = %d, want 1", got)
	}
	fmt.Println("BOOMPI_EXTERNAL_WSS_UPLINK_COMMITTED_AND_SESSION_CLOSED")
}

func writeFutureServerIdentity(t *testing.T, directory string) {
	t.Helper()
	privateKey, err := ecdsa.GenerateKey(elliptic.P256(), rand.Reader)
	if err != nil {
		t.Fatalf("generate future TLS private key: %v", err)
	}
	now := time.Now()
	template := x509.Certificate{
		SerialNumber: big.NewInt(1),
		Subject:      pkix.Name{CommonName: "boomPI future-validity smoke"},
		NotBefore:    now.Add(24 * time.Hour),
		NotAfter:     now.Add(7 * 24 * time.Hour),
		KeyUsage:     x509.KeyUsageDigitalSignature,
		ExtKeyUsage:  []x509.ExtKeyUsage{x509.ExtKeyUsageServerAuth},
		DNSNames:     []string{"boompi.test"},
	}
	certificateDER, err := x509.CreateCertificate(
		rand.Reader, &template, &template, &privateKey.PublicKey, privateKey)
	if err != nil {
		t.Fatalf("create future TLS certificate: %v", err)
	}
	privateKeyDER, err := x509.MarshalPKCS8PrivateKey(privateKey)
	if err != nil {
		t.Fatalf("encode future TLS private key: %v", err)
	}
	certificatePEM := pem.EncodeToMemory(
		&pem.Block{Type: "CERTIFICATE", Bytes: certificateDER})
	privateKeyPEM := pem.EncodeToMemory(
		&pem.Block{Type: "PRIVATE KEY", Bytes: privateKeyDER})
	if err := os.WriteFile(filepath.Join(directory, "server.crt"), certificatePEM, 0o600); err != nil {
		t.Fatalf("write future TLS certificate: %v", err)
	}
	if err := os.WriteFile(filepath.Join(directory, "server.key"), privateKeyPEM, 0o600); err != nil {
		t.Fatalf("write future TLS private key: %v", err)
	}
}

func waitForTCPListener(address string, timeout time.Duration) error {
	deadline := time.Now().Add(timeout)
	for {
		connection, err := net.DialTimeout("tcp", address, 100*time.Millisecond)
		if err == nil {
			_ = connection.Close()
			return nil
		}
		if time.Now().After(deadline) {
			return fmt.Errorf("listener %s was not ready: %w", address, err)
		}
		time.Sleep(10 * time.Millisecond)
	}
}

func smokeChildEnvironment(deviceToken string) []string {
	environment := make([]string, 0, len(os.Environ())+1)
	for _, entry := range os.Environ() {
		key, _, _ := strings.Cut(entry, "=")
		if strings.EqualFold(key, "BOOMPI_DEVICE_TOKEN") ||
			strings.EqualFold(key, "DASHSCOPE_API_KEY") ||
			strings.EqualFold(key, "DASHSCOPE_WORKSPACE_ID") {
			continue
		}
		environment = append(environment, entry)
	}
	return append(environment, "BOOMPI_DEVICE_TOKEN="+deviceToken)
}

func manualSingleTurnChildEnvironment(cfg config.Config, spkiPin string) []string {
	overrides := map[string]string{
		"BOOMPI_SERVER_IP":            cfg.ListenAddress,
		"BOOMPI_SERVER_PORT":          strconv.Itoa(cfg.WSSPort),
		"BOOMPI_SERVER_NAME":          "boompi.test",
		"BOOMPI_SERVER_SPKI_SHA256":   spkiPin,
		"BOOMPI_DEVICE_ID":            testDeviceID,
		"BOOMPI_DEVICE_TOKEN":         testDeviceToken,
		"BOOMPI_CAPTURE_PCM":          "null",
		"BOOMPI_PLAYBACK_PCM":         "null",
		"BOOMPI_CAPTURE_MIC_SLOT":     "0",
		"BOOMPI_CAPTURE_MIC_POLARITY": "1",
		"BOOMPI_MANUAL_RECORD_MS":     "3000",
		"BOOMPI_VOLUME_PERCENT":       "60",
		"BOOMPI_SPEAKER_GAIN_PERCENT": "100",
	}
	blocked := make(map[string]struct{}, len(overrides)+2)
	for key := range overrides {
		blocked[strings.ToUpper(key)] = struct{}{}
	}
	blocked["DASHSCOPE_API_KEY"] = struct{}{}
	blocked["DASHSCOPE_WORKSPACE_ID"] = struct{}{}

	environment := make([]string, 0, len(os.Environ())+len(overrides))
	for _, entry := range os.Environ() {
		key, _, _ := strings.Cut(entry, "=")
		if _, found := blocked[strings.ToUpper(key)]; found {
			continue
		}
		environment = append(environment, entry)
	}
	for key, value := range overrides {
		environment = append(environment, key+"="+value)
	}
	return environment
}

func minInt(left, right int) int {
	if left < right {
		return left
	}
	return right
}
