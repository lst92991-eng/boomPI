package qwen

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net"
	"net/http"
	"net/url"
	"regexp"
	"strings"
	"time"

	"github.com/gorilla/websocket"
	"github.com/lst92991-eng/boomPI/server/internal/backend"
)

const (
	maxAudioChunkBytes = 256 * 1024
	maxMessageBytes    = 8 * 1024 * 1024
	RegionChinaBeijing = "china-beijing"
	RegionSingapore    = "singapore"
)

var workspaceIDPattern = regexp.MustCompile(`^[A-Za-z0-9_-]+$`)

// Config does not load defaults or secrets; the application supplies both.
type Config struct {
	APIKey      string
	WorkspaceID string
	Region      string
	Endpoint    string
	Model       string
	Voice       string
	Timeout     time.Duration
	QueueSize   int
}

func (c Config) Validate() error {
	if strings.TrimSpace(c.APIKey) == "" {
		return errors.New("qwen API key is required")
	}
	if strings.TrimSpace(c.Model) == "" || strings.TrimSpace(c.Voice) == "" {
		return errors.New("qwen model and voice are required")
	}
	if c.Endpoint == "" {
		if _, err := RegionalEndpoint(c.Region, c.WorkspaceID); err != nil {
			return err
		}
	} else if err := validateEndpoint(c.Endpoint); err != nil {
		return err
	}
	if c.Timeout <= 0 || c.Timeout > 2*time.Minute {
		return errors.New("qwen timeout must be greater than zero and at most 2m")
	}
	if c.QueueSize < 1 || c.QueueSize > 4096 {
		return errors.New("qwen queue size must be between 1 and 4096")
	}
	return nil
}

// RegionalEndpoint builds a workspace-specific Model Studio Realtime URL.
func RegionalEndpoint(region, workspaceID string) (string, error) {
	workspaceID = strings.TrimSpace(workspaceID)
	if !workspaceIDPattern.MatchString(workspaceID) {
		return "", errors.New("qwen workspace ID is required and contains invalid characters")
	}
	var domain string
	switch strings.ToLower(strings.TrimSpace(region)) {
	case RegionChinaBeijing:
		domain = "cn-beijing.maas.aliyuncs.com"
	case RegionSingapore:
		domain = "ap-southeast-1.maas.aliyuncs.com"
	default:
		return "", fmt.Errorf("unsupported qwen region %q", region)
	}
	return fmt.Sprintf("wss://%s.%s/api-ws/v1/realtime", workspaceID, domain), nil
}

var (
	ErrProvider  = errors.New("qwen provider error")
	ErrTransport = errors.New("qwen transport error")
	ErrCanceled  = errors.New("qwen operation canceled")
)

func transportError(err error) error {
	kind := ErrTransport
	if errors.Is(err, context.Canceled) {
		kind = ErrCanceled
	}
	return fmt.Errorf("%w: %w", kind, err)
}

type Backend struct {
	config Config
	dialer *websocket.Dialer
}

var _ backend.ConversationBackend = (*Backend)(nil)

func New(config Config) (*Backend, error) {
	config.APIKey = strings.TrimSpace(config.APIKey)
	config.WorkspaceID = strings.TrimSpace(config.WorkspaceID)
	config.Region = strings.ToLower(strings.TrimSpace(config.Region))
	config.Endpoint = strings.TrimSpace(config.Endpoint)
	config.Model = strings.TrimSpace(config.Model)
	config.Voice = strings.TrimSpace(config.Voice)
	if err := config.Validate(); err != nil {
		return nil, fmt.Errorf("configure qwen: %w", err)
	}
	dialer := *websocket.DefaultDialer
	dialer.HandshakeTimeout = config.Timeout
	return &Backend{config: config, dialer: &dialer}, nil
}

func (b *Backend) Open(ctx context.Context, cfg backend.SessionConfig) (backend.ConversationSession, error) {
	if ctx == nil {
		ctx = context.Background()
	}
	endpoint, err := b.config.endpointURL()
	if err != nil {
		return nil, fmt.Errorf("build qwen endpoint: %w", err)
	}
	header := make(http.Header)
	header.Set("Authorization", "Bearer "+b.config.APIKey)
	conn, response, err := b.dialer.DialContext(ctx, endpoint, header)
	responseStatus := ""
	responseDetail := ""
	if response != nil {
		responseStatus = safeHTTPStatus(response.StatusCode)
		if err != nil {
			responseDetail = readHandshakeError(response.Body, b.config.APIKey)
		}
	}
	if err != nil {
		if responseStatus != "" {
			if responseDetail != "" {
				return nil, fmt.Errorf("%w: Qwen handshake returned %s (%s)", transportError(err), responseStatus, responseDetail)
			}
			return nil, fmt.Errorf("%w: Qwen handshake returned %s", transportError(err), responseStatus)
		}
		return nil, transportError(err)
	}

	s := newSession(b.config, conn)
	s.start()
	if err := s.enqueue(ctx, outboundBatch{events: []clientEvent{newSessionUpdate(b.config, cfg)}}); err != nil {
		_ = s.Close()
		return nil, err
	}

	timer := time.NewTimer(b.config.Timeout)
	defer timer.Stop()
	select {
	case readyErr := <-s.ready:
		if readyErr != nil {
			_ = s.Close()
			return nil, readyErr
		}
		return s, nil
	case <-ctx.Done():
		_ = s.Close()
		return nil, transportError(ctx.Err())
	case <-timer.C:
		_ = s.Close()
		return nil, transportError(context.DeadlineExceeded)
	}
}

func safeHTTPStatus(statusCode int) string {
	if text := http.StatusText(statusCode); text != "" {
		return fmt.Sprintf("%d %s", statusCode, text)
	}
	return fmt.Sprint(statusCode)
}

func readHandshakeError(body io.ReadCloser, apiKey string) string {
	if body == nil {
		return ""
	}
	defer body.Close()
	data, err := io.ReadAll(io.LimitReader(body, 4*1024))
	if err != nil {
		return ""
	}
	var response struct {
		Code    string `json:"code"`
		Message string `json:"message"`
		Error   string `json:"error"`
	}
	if json.Unmarshal(data, &response) != nil {
		return ""
	}
	message := response.Message
	if message == "" {
		message = response.Error
	}
	response.Code = strings.ReplaceAll(response.Code, apiKey, "<redacted>")
	message = strings.ReplaceAll(message, apiKey, "<redacted>")
	if response.Code != "" && message != "" {
		return "code=" + response.Code + " message=" + message
	}
	if response.Code != "" {
		return "code=" + response.Code
	}
	return message
}

func (c Config) endpointURL() (string, error) {
	endpoint := c.Endpoint
	if endpoint == "" {
		var err error
		endpoint, err = RegionalEndpoint(c.Region, c.WorkspaceID)
		if err != nil {
			return "", err
		}
	}
	u, err := url.Parse(endpoint)
	if err != nil {
		return "", err
	}
	query := u.Query()
	query.Set("model", c.Model)
	u.RawQuery = query.Encode()
	return u.String(), nil
}

func validateEndpoint(endpoint string) error {
	u, err := url.Parse(strings.TrimSpace(endpoint))
	if err != nil {
		return fmt.Errorf("parse qwen endpoint: %w", err)
	}
	if u.Host == "" || (u.Scheme != "wss" && u.Scheme != "ws") {
		return errors.New("qwen endpoint must be an absolute ws or wss URL")
	}
	if u.User != nil || u.Fragment != "" {
		return errors.New("qwen endpoint must not contain credentials or a fragment")
	}
	if u.Scheme == "ws" && !isLoopbackHost(u.Hostname()) {
		return errors.New("unencrypted qwen endpoint is allowed only on loopback")
	}
	return nil
}

func isLoopbackHost(host string) bool {
	if strings.EqualFold(host, "localhost") {
		return true
	}
	ip := net.ParseIP(host)
	return ip != nil && ip.IsLoopback()
}

func newSessionUpdate(config Config, cfg backend.SessionConfig) clientEvent {
	instructions := strings.TrimSpace(cfg.SystemPrompt)
	if persona := strings.TrimSpace(cfg.Persona); persona != "" {
		if instructions != "" {
			instructions += "\n\n"
		}
		instructions += persona
	}
	return clientEvent{
		Type: "session.update",
		Session: &sessionUpdate{
			Modalities:        []string{"text", "audio"},
			Voice:             config.Voice,
			Instructions:      instructions,
			InputAudioFormat:  "pcm",
			OutputAudioFormat: "pcm",
			TurnDetection:     nil,
		},
	}
}
