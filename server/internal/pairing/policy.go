package pairing

import (
	"errors"
	"time"
)

type State uint8

const (
	StateClosed State = iota
	StateAwaitingConfirmation
	StatePaired
)

type Policy struct {
	CodeDigits  int
	CodeTTL     time.Duration
	MaxAttempts int
}

func DefaultPolicy() Policy {
	return Policy{CodeDigits: 6, CodeTTL: 2 * time.Minute, MaxAttempts: 5}
}

func (p Policy) Validate() error {
	if p.CodeDigits != 6 {
		return errors.New("pairing code must contain six digits")
	}
	if p.CodeTTL < 30*time.Second || p.CodeTTL > 10*time.Minute {
		return errors.New("pairing code TTL must be between 30s and 10m")
	}
	if p.MaxAttempts < 1 || p.MaxAttempts > 10 {
		return errors.New("pairing attempts must be between 1 and 10")
	}
	return nil
}
