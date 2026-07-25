package update

import "errors"

type Policy struct {
	LANOnly          bool
	RequirePairing   bool
	RequireSignature bool
	MaxPackageBytes  int64
}

func DefaultPolicy() Policy {
	return Policy{
		LANOnly: true, RequirePairing: true, RequireSignature: true,
		MaxPackageBytes: 256 * 1024 * 1024,
	}
}

func (p Policy) Validate() error {
	if !p.LANOnly || !p.RequirePairing || !p.RequireSignature {
		return errors.New("updates must be LAN-only, paired, and signed")
	}
	if p.MaxPackageBytes <= 0 {
		return errors.New("update package size limit must be positive")
	}
	return nil
}
