package app

import (
	"context"
	"errors"
	"fmt"
	"log/slog"
	"net"

	"github.com/lst92991-eng/boomPI/server/internal/backend"
	"github.com/lst92991-eng/boomPI/server/internal/backend/qwen"
	"github.com/lst92991-eng/boomPI/server/internal/config"
	"github.com/lst92991-eng/boomPI/server/internal/identity"
	"github.com/lst92991-eng/boomPI/server/internal/transport"
)

// App is the deliberately small server MVP: one WSS listener, one device and
// one pluggable conversation backend.
type App struct {
	cfg       config.Config
	logger    *slog.Logger
	transport *transport.Server
	spkiPin   string
}

func New(cfg config.Config, logger *slog.Logger, identityDirectory string) (*App, error) {
	if logger == nil {
		return nil, errors.New("logger is required")
	}
	if err := cfg.Validate(); err != nil {
		return nil, err
	}
	provider, err := qwen.New(qwen.Config{
		APIKey:      cfg.Credentials.APIKey(),
		WorkspaceID: cfg.Credentials.WorkspaceID(),
		Model:       cfg.Model,
		Voice:       cfg.Voice,
		Timeout:     cfg.ConnectionTimeout,
		QueueSize:   32,
	})
	if err != nil {
		return nil, err
	}
	return newWithBackend(cfg, logger, identityDirectory, provider)
}

func newWithBackend(cfg config.Config, logger *slog.Logger, identityDirectory string, provider backend.ConversationBackend) (*App, error) {
	if logger == nil || provider == nil {
		return nil, errors.New("logger and conversation backend are required")
	}
	if err := cfg.Validate(); err != nil {
		return nil, err
	}
	serverIdentity, err := identity.LoadOrCreate(identityDirectory)
	if err != nil {
		return nil, fmt.Errorf("load server TLS identity: %w", err)
	}
	handler := &deviceHandler{cfg: cfg, logger: logger, provider: provider}
	wss, err := transport.NewServer(transport.Config{
		Address:      net.JoinHostPort(cfg.ListenAddress, fmt.Sprint(cfg.WSSPort)),
		TLSConfig:    serverIdentity.TLSConfig,
		PingInterval: cfg.HeartbeatInterval,
		PongTimeout:  cfg.ConnectionTimeout,
	}, handler)
	if err != nil {
		return nil, err
	}
	return &App{cfg: cfg, logger: logger, transport: wss, spkiPin: serverIdentity.SPKISHA256}, nil
}

func (a *App) Run(ctx context.Context) error {
	if ctx == nil {
		return errors.New("context is required")
	}
	a.logger.Info("boomPI server started",
		"wss_address", net.JoinHostPort(a.cfg.ListenAddress, fmt.Sprint(a.cfg.WSSPort)),
		"provider", a.cfg.Provider,
		"region", a.cfg.Region,
		"model", a.cfg.Model,
		"voice", a.cfg.Voice,
		"credential_source", a.cfg.Credentials.Source(),
		"tls_spki_sha256", a.spkiPin,
	)
	err := a.transport.Serve(ctx)
	if err == nil || ctx.Err() != nil {
		a.logger.Info("boomPI server stopped")
		return nil
	}
	return err
}
