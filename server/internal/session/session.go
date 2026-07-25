package session

import (
	"errors"
	"time"
)

type State uint8

const (
	StateStarting State = iota
	StateIdle
	StateListening
	StateThinking
	StateSpeaking
	StateFollowUp
	StateRecovering
	StateClosed
)

type Limits struct {
	IdleTimeout      time.Duration
	MaxTurns         int
	MaxContextTokens int
}

func (l Limits) Validate() error {
	if l.IdleTimeout < time.Minute || l.IdleTimeout > 24*time.Hour {
		return errors.New("session idle timeout must be between 1m and 24h")
	}
	if l.MaxTurns < 1 || l.MaxTurns > 100 {
		return errors.New("session max turns must be between 1 and 100")
	}
	if l.MaxContextTokens < 1024 || l.MaxContextTokens > 1_000_000 {
		return errors.New("session max context tokens must be between 1024 and 1000000")
	}
	return nil
}
