package app

import (
	"context"
	"errors"
	"fmt"
	"log/slog"
	"net"

	"github.com/lst92991-eng/boomPI/server/internal/backend"
	"github.com/lst92991-eng/boomPI/server/internal/backend/qwenpipeline"
	"github.com/lst92991-eng/boomPI/server/internal/config"
	"github.com/lst92991-eng/boomPI/server/internal/discovery"
	"github.com/lst92991-eng/boomPI/server/internal/identity"
	"github.com/lst92991-eng/boomPI/server/internal/transport"
)

const providerEventQueueCapacity = 8

// App is the deliberately small server: one WSS listener, one device and
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
	return qwenpipeline.New(qwenpipeline.Config{
		APIKey:           cfg.Credentials.APIKey(),
		WorkspaceID:      cfg.Credentials.WorkspaceID(),
		Region:           qwenpipeline.RegionChinaBeijing,
		ASRModel:         cfg.ASRModel,
		ReasoningModel:   cfg.ReasoningModel,
		ReasoningEffort:  cfg.ReasoningEffort,
		TTSModel:         cfg.TTSModel,
		TTSVoice:         cfg.TTSVoice,
		SearchMode:       cfg.SearchMode,
		Timeout:          cfg.FirstResponseTimeout,
		QueueSize:        providerEventQueueCapacity,
		MaxTurns:         cfg.MaxTurns,
		MaxContextTokens: cfg.MaxContextTokens,
		Logger:           logger,
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
		"region", qwenpipeline.RegionChinaBeijing,
		"asr_model", a.cfg.ASRModel,
		"reasoning_model", a.cfg.ReasoningModel,
		"reasoning_effort", a.cfg.ReasoningEffort,
		"search_mode", a.cfg.SearchMode,
		"tts_model", a.cfg.TTSModel,
		"tts_voice", a.cfg.TTSVoice,
		"credential_source", a.cfg.Credentials.Source(),
		"tls_spki_sha256", a.spkiPin,
	)
	err := runListeners(ctx, a.transport.Serve, a.discovery.Serve)
	if err == nil && ctx.Err() != nil {
		a.logger.Info("boomPI server stopped")
	}
	return err
}

func runListeners(
	ctx context.Context,
	serveWSS func(context.Context) error,
	serveDiscovery func(context.Context) error,
) error {
	runCtx, stop := context.WithCancel(ctx)
	defer stop()
	results := make(chan error, 2)
	go func() { results <- serveWSS(runCtx) }()
	go func() { results <- serveDiscovery(runCtx) }()
	first := <-results
	// Both listeners are lifetime services: a nil result is valid only after
	// the parent requested shutdown. Treat any earlier nil as a real failure so
	// a silently stopped UDP or WSS listener cannot look like a clean restart.
	if first == nil && ctx.Err() == nil {
		first = errors.New("server listener stopped without a shutdown request")
	}
	stop()
	second := <-results
	if ctx.Err() != nil {
		return nil
	}
	if first != nil {
		return first
	}
	return second
}
