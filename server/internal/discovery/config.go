package discovery

import (
	"errors"
	"net"
)

const ProtocolVersion = 1

type Config struct {
	BindAddress string
	UDPPort     int
	WSSPort     int
}

func (c Config) Validate() error {
	if net.ParseIP(c.BindAddress) == nil {
		return errors.New("discovery bind address must be an IP address")
	}
	if c.UDPPort < 1 || c.UDPPort > 65535 || c.WSSPort < 1 || c.WSSPort > 65535 {
		return errors.New("discovery ports must be between 1 and 65535")
	}
	if c.UDPPort == c.WSSPort {
		return errors.New("discovery UDP and WSS ports must differ")
	}
	return nil
}

type Advertisement struct {
	Version      int      `json:"version"`
	ServerID     string   `json:"server_id"`
	WSSPort      int      `json:"wss_port"`
	Capabilities []string `json:"capabilities"`
}
