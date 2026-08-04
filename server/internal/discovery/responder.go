package discovery

import (
	"bytes"
	"context"
	"encoding/base64"
	"errors"
	"fmt"
	"net"
)

const (
	Request  = "BOOMPI_DISCOVER_V1"
	Response = "BOOMPI_SERVER_V1"
)

const maxDiscoveryRequestBytes = 64

type Responder struct {
	config  Config
	payload []byte
}

func NewResponder(config Config, spkiSHA256 string) (*Responder, error) {
	if err := config.Validate(); err != nil {
		return nil, err
	}
	digest, err := base64.StdEncoding.DecodeString(spkiSHA256)
	if err != nil || len(digest) != 32 {
		return nil, errors.New("discovery SPKI must be a base64-encoded SHA-256 digest")
	}
	payload := []byte(fmt.Sprintf("%s %d %s", Response, config.WSSPort, spkiSHA256))
	return &Responder{config: config, payload: payload}, nil
}

// Serve answers one fixed, non-secret discovery request until ctx is canceled.
func (r *Responder) Serve(ctx context.Context) error {
	if ctx == nil {
		return errors.New("discovery context is required")
	}
	address := &net.UDPAddr{IP: net.ParseIP(r.config.BindAddress), Port: r.config.UDPPort}
	connection, err := net.ListenUDP("udp", address)
	if err != nil {
		return fmt.Errorf("listen for UDP discovery: %w", err)
	}
	defer connection.Close()
	return r.serveConnection(ctx, connection)
}

func (r *Responder) serveConnection(ctx context.Context, connection *net.UDPConn) error {
	if ctx == nil || connection == nil {
		return errors.New("discovery context and connection are required")
	}
	watchDone := make(chan struct{})
	go func() {
		select {
		case <-ctx.Done():
			_ = connection.Close()
		case <-watchDone:
		}
	}()
	defer close(watchDone)

	request := make([]byte, maxDiscoveryRequestBytes)
	for {
		count, peer, err := connection.ReadFromUDP(request)
		if err != nil {
			if ctx.Err() != nil {
				return nil
			}
			// UDP 读超时或临时网络错误不代表 WSS 服务失效；继续监听即可。
			if networkError, ok := err.(net.Error); ok && (networkError.Timeout() || networkError.Temporary()) {
				continue
			}
			return fmt.Errorf("read UDP discovery request: %w", err)
		}
		if !bytes.Equal(request[:count], []byte(Request)) {
			continue
		}
		if _, err := connection.WriteToUDP(r.payload, peer); err != nil {
			if ctx.Err() != nil {
				return nil
			}
			// UDP 没有会话，某个请求方在响应前离线不应拖垮持久 WSS 服务。
			continue
		}
	}
}
