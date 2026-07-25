package app

import (
	"context"
	"errors"
	"log/slog"

	"github.com/lst92991-eng/boomPI/server/internal/backend"
	"github.com/lst92991-eng/boomPI/server/internal/config"
	"github.com/lst92991-eng/boomPI/server/internal/discovery"
	"github.com/lst92991-eng/boomPI/server/internal/pairing"
	"github.com/lst92991-eng/boomPI/server/internal/session"
	"github.com/lst92991-eng/boomPI/server/internal/tools"
	"github.com/lst92991-eng/boomPI/server/internal/update"
)

type App struct {
	cfg      config.Config
	logger   *slog.Logger
	backends *backend.Registry
	tools    *tools.Registry
}

func New(cfg config.Config, logger *slog.Logger) (*App, error) {
	if logger == nil {
		return nil, errors.New("logger is required")
	}
	if err := cfg.Validate(); err != nil {
		return nil, err
	}
	if err := (discovery.Config{BindAddress: cfg.ListenAddress, UDPPort: cfg.DiscoveryPort, WSSPort: cfg.WSSPort}).Validate(); err != nil {
		return nil, err
	}
	pairingPolicy := pairing.DefaultPolicy()
	pairingPolicy.CodeTTL = cfg.PairingCodeTTL
	pairingPolicy.MaxAttempts = cfg.PairingMaxAttempts
	if err := pairingPolicy.Validate(); err != nil {
		return nil, err
	}
	if err := (session.Limits{IdleTimeout: cfg.SessionIdleTimeout, MaxTurns: cfg.MaxTurns, MaxContextTokens: cfg.MaxContextTokens}).Validate(); err != nil {
		return nil, err
	}
	if err := update.DefaultPolicy().Validate(); err != nil {
		return nil, err
	}
	return &App{cfg: cfg, logger: logger, backends: backend.NewRegistry(), tools: tools.NewRegistry()}, nil
}

func (a *App) Run(ctx context.Context) error {
	if ctx == nil {
		return errors.New("context is required")
	}
	a.logger.Info("boomPI P1 runtime started",
		"listen_address", a.cfg.ListenAddress,
		"wss_port", a.cfg.WSSPort,
		"discovery_port", a.cfg.DiscoveryPort,
		"provider", a.cfg.Provider,
		"region", a.cfg.Region,
		"model", a.cfg.Model,
		"credential_source", a.cfg.Credentials.Source(),
		"network_listeners_enabled", false,
		"qwen_connected", false,
	)
	<-ctx.Done()
	a.logger.Info("boomPI P1 runtime stopping", "reason", "context canceled")
	return nil
}
