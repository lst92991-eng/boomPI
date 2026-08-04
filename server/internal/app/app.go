package app

import (
	"context"
	"errors"
	"fmt"
	"log/slog"
	"net"

	"github.com/lst92991-eng/boomPI/server/internal/backend"
	"github.com/lst92991-eng/boomPI/server/internal/backend/qwen"
	"github.com/lst92991-eng/boomPI/server/internal/backend/qwenpipeline"
	"github.com/lst92991-eng/boomPI/server/internal/config"
	"github.com/lst92991-eng/boomPI/server/internal/discovery"
	"github.com/lst92991-eng/boomPI/server/internal/identity"
	"github.com/lst92991-eng/boomPI/server/internal/transport"
)

// App is the deliberately small server MVP: one WSS listener, one device and
// one pluggable conversation backend.
type App struct {
	cfg       config.Config
	logger    *slog.Logger
	transport *transport.Server
	discovery *discovery.Responder
	spkiPin   string
}

func New(cfg config.Config, logger *slog.Logger, identityDirectory string) (*App, error) {
	if logger == nil {
		return nil, errors.New("logger is required")
	}
	if err := cfg.Validate(); err != nil {
		return nil, err
	}
	provider, err := newQwenBackend(cfg, logger)
	if err != nil {
		return nil, err
	}
	return newWithBackend(cfg, logger, identityDirectory, provider)
}

func newQwenBackend(cfg config.Config, logger *slog.Logger) (backend.ConversationBackend, error) {
	if cfg.ConversationMode == "intelligence" {
		return qwenpipeline.New(qwenpipeline.Config{
			APIKey:           cfg.Credentials.APIKey(),
			WorkspaceID:      cfg.Credentials.WorkspaceID(),
			Region:           cfg.Region,
			ASRModel:         cfg.ASRModel,
			ReasoningModel:   cfg.ReasoningModel,
			ReasoningEffort:  cfg.ReasoningEffort,
			TTSModel:         cfg.TTSModel,
			TTSVoice:         cfg.TTSVoice,
			SearchMode:       cfg.SearchMode,
			Timeout:          cfg.FirstResponseTimeout,
			QueueSize:        64,
			MaxTurns:         cfg.MaxTurns,
			MaxContextTokens: cfg.MaxContextTokens,
			Logger:           logger,
		})
	}
	return qwen.New(qwen.Config{
		APIKey:      cfg.Credentials.APIKey(),
		WorkspaceID: cfg.Credentials.WorkspaceID(),
		Region:      cfg.Region,
		Model:       cfg.Model,
		Voice:       cfg.Voice,
		Timeout:     cfg.ConnectionTimeout,
		QueueSize:   32,
	})
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
	udpDiscovery, err := discovery.NewResponder(discovery.Config{
		BindAddress: cfg.ListenAddress,
		UDPPort:     cfg.DiscoveryPort,
		WSSPort:     cfg.WSSPort,
	}, serverIdentity.SPKISHA256)
	if err != nil {
		return nil, fmt.Errorf("configure UDP discovery: %w", err)
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
	return &App{
		cfg: cfg, logger: logger, transport: wss, discovery: udpDiscovery,
		spkiPin: serverIdentity.SPKISHA256,
	}, nil
}

func (a *App) Run(ctx context.Context) error {
	if ctx == nil {
		return errors.New("context is required")
	}
	a.logger.Info("boomPI server starting",
		"wss_address", net.JoinHostPort(a.cfg.ListenAddress, fmt.Sprint(a.cfg.WSSPort)),
		"discovery_address", net.JoinHostPort(a.cfg.ListenAddress, fmt.Sprint(a.cfg.DiscoveryPort)),
		"provider", a.cfg.Provider,
		"region", a.cfg.Region,
		"conversation_mode", a.cfg.ConversationMode,
		"model", a.cfg.Model,
		"reasoning_model", a.cfg.ReasoningModel,
		"reasoning_effort", a.cfg.ReasoningEffort,
		"search_mode", a.cfg.SearchMode,
		"voice", a.cfg.Voice,
		"credential_source", a.cfg.Credentials.Source(),
		"tls_spki_sha256", a.spkiPin,
	)
	runCtx, stop := context.WithCancel(ctx)
	defer stop()
	results := make(chan error, 2)
	go func() { results <- a.transport.Serve(runCtx) }()
	go func() { results <- a.discovery.Serve(runCtx) }()
	first := <-results
	stop()
	second := <-results
	if ctx.Err() != nil {
		a.logger.Info("boomPI server stopped")
		return nil
	}
	if first != nil {
		return first
	}
	return second
}
