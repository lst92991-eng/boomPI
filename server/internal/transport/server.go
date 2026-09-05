package transport

import (
	"context"
	"crypto/tls"
	"errors"
	"fmt"
	"net"
	"net/http"
	"sync"
	"sync/atomic"
	"time"

	"github.com/gorilla/websocket"
)

const (
	defaultAddress      = ":17806"
	defaultPath         = "/ws"
	defaultPingInterval = 10 * time.Second
	defaultPongTimeout  = 30 * time.Second
	maxPingInterval     = 10 * time.Second
	maxPongTimeout      = 30 * time.Second
	writeTimeout        = 5 * time.Second
	sendQueueCapacity   = 16
)

// Config contains the bounded network and lifetime settings owned by Server.
// TLSConfig is mandatory; the transport never falls back to plaintext HTTP.
type Config struct {
	Address      string
	TLSConfig    *tls.Config
	PingInterval time.Duration
	PongTimeout  time.Duration
}

// Handler owns the business lifetime of one connected device. It must return
// when ctx is cancelled. WebSocket details remain private to this package.
type Handler interface {
	Handle(ctx context.Context, connection *Connection) error
}

// Server accepts one active device on one TLS-only WebSocket endpoint.
type Server struct {
	config   Config
	handler  Handler
	upgrader websocket.Upgrader

	active    atomic.Bool
	activeMu  sync.Mutex
	activeCon *Connection

	lifecycleCtx    context.Context
	lifecycleCancel context.CancelCauseFunc
	serveStarted    atomic.Bool
}

// NewServer validates the TLS-only transport configuration.
func NewServer(config Config, handler Handler) (*Server, error) {
	if handler == nil {
		return nil, errors.New("transport handler is required")
	}
	normalized, err := normalizeConfig(config)
	if err != nil {
		return nil, err
	}
	lifecycleCtx, lifecycleCancel := context.WithCancelCause(context.Background())
	return &Server{
		config:  normalized,
		handler: handler,
		upgrader: websocket.Upgrader{
			ReadBufferSize:  4096,
			WriteBufferSize: 4096,
			CheckOrigin: func(request *http.Request) bool {
				// The board is not a browser. Reject browser-originated upgrades by default.
				return request.Header.Get("Origin") == ""
			},
		},
		lifecycleCtx:    lifecycleCtx,
		lifecycleCancel: lifecycleCancel,
	}, nil
}

// Serve listens until ctx is cancelled or the listener fails. It can be called once.
func (server *Server) Serve(ctx context.Context) error {
	if ctx == nil {
		return errors.New("serve context is required")
	}
	if !server.serveStarted.CompareAndSwap(false, true) {
		return errors.New("transport server can only be served once")
	}

	listener, err := net.Listen("tcp", server.config.Address)
	if err != nil {
		return fmt.Errorf("listen for WSS: %w", err)
	}
	tlsListener := tls.NewListener(listener, server.config.TLSConfig.Clone())
	httpServer := &http.Server{
		Handler:           http.HandlerFunc(server.serveHTTP),
		ReadHeaderTimeout: server.config.PongTimeout,
	}

	watchCtx, stopWatch := context.WithCancel(ctx)
	watchDone := make(chan struct{})
	go func() {
		defer close(watchDone)
		<-watchCtx.Done()
		server.lifecycleCancel(context.Cause(watchCtx))
		server.closeActive()
		shutdownCtx, cancel := context.WithTimeout(context.Background(), writeTimeout)
		defer cancel()
		_ = httpServer.Shutdown(shutdownCtx)
	}()

	serveErr := httpServer.Serve(tlsListener)
	stopWatch()
	<-watchDone
	server.closeActive()
	if errors.Is(serveErr, http.ErrServerClosed) && ctx.Err() != nil {
		return nil
	}
	if serveErr != nil {
		return fmt.Errorf("serve WSS: %w", serveErr)
	}
	return nil
}

func normalizeConfig(config Config) (Config, error) {
	if config.TLSConfig == nil {
		return Config{}, errors.New("TLS configuration is required")
	}
	if len(config.TLSConfig.Certificates) == 0 && config.TLSConfig.GetCertificate == nil && config.TLSConfig.GetConfigForClient == nil {
		return Config{}, errors.New("TLS configuration must provide a server certificate")
	}
	if config.TLSConfig.MinVersion != 0 && config.TLSConfig.MinVersion < tls.VersionTLS12 {
		return Config{}, errors.New("TLS minimum version must be TLS 1.2 or newer")
	}
	if config.TLSConfig.MaxVersion != 0 && config.TLSConfig.MaxVersion < tls.VersionTLS12 {
		return Config{}, errors.New("TLS maximum version must allow TLS 1.2 or newer")
	}
	config.TLSConfig = config.TLSConfig.Clone()
	if config.TLSConfig.MinVersion == 0 {
		config.TLSConfig.MinVersion = tls.VersionTLS12
	}
	if config.Address == "" {
		config.Address = defaultAddress
	}
	if config.PingInterval == 0 {
		config.PingInterval = defaultPingInterval
	}
	if config.PongTimeout == 0 {
		config.PongTimeout = defaultPongTimeout
	}
	if config.PingInterval < time.Second || config.PingInterval > maxPingInterval {
		return Config{}, errors.New("ping interval must be between 1s and 10s")
	}
	if config.PongTimeout < 3*config.PingInterval || config.PongTimeout > maxPongTimeout {
		return Config{}, errors.New("pong timeout must cover at least three ping intervals and be at most 30s")
	}
	return config, nil
}

func (server *Server) serveHTTP(response http.ResponseWriter, request *http.Request) {
	if request.TLS == nil {
		http.Error(response, "TLS is required", http.StatusUpgradeRequired)
		return
	}
	if request.Method != http.MethodGet || request.URL.Path != defaultPath {
		http.NotFound(response, request)
		return
	}
	if !server.active.CompareAndSwap(false, true) {
		http.Error(response, "one device is already connected", http.StatusServiceUnavailable)
		return
	}
	defer server.active.Store(false)

	webSocket, err := server.upgrader.Upgrade(response, request, nil)
	if err != nil {
		return
	}
	connection, err := newConnection(server.lifecycleCtx, webSocket, server.config)
	if err != nil {
		_ = webSocket.Close()
		return
	}
	server.setActive(connection)
	defer func() {
		_ = connection.Close()
		server.clearActive(connection)
	}()

	_ = server.handler.Handle(connection.context(), connection)
}

func (server *Server) setActive(connection *Connection) {
	server.activeMu.Lock()
	server.activeCon = connection
	server.activeMu.Unlock()
}

func (server *Server) clearActive(connection *Connection) {
	server.activeMu.Lock()
	if server.activeCon == connection {
		server.activeCon = nil
	}
	server.activeMu.Unlock()
}

func (server *Server) closeActive() {
	server.activeMu.Lock()
	connection := server.activeCon
	server.activeMu.Unlock()
	if connection != nil {
		_ = connection.Close()
	}
}
