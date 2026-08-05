package discovery

import (
	"errors"
	"net"
)

type Config struct {
	BindAddress string
	UDPPort     int
	WSSPort     int
}

func (c Config) Validate() error {
	if ip := net.ParseIP(c.BindAddress); ip == nil || ip.To4() == nil {
		return errors.New("discovery bind address must be an IPv4 address")
	}
	if c.UDPPort < 1 || c.UDPPort > 65535 || c.WSSPort < 1 || c.WSSPort > 65535 {
		return errors.New("discovery ports must be between 1 and 65535")
	}
	if c.UDPPort == c.WSSPort {
		return errors.New("discovery UDP and WSS ports must differ")
	}
	return nil
}
