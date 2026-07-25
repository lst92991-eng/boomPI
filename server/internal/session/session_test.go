package session

import (
	"testing"
	"time"
)

func TestLimitsValidate(t *testing.T) {
	limits := Limits{IdleTimeout: 30 * time.Minute, MaxTurns: 20, MaxContextTokens: 24_000}
	if err := limits.Validate(); err != nil {
		t.Fatalf("Validate() error = %v", err)
	}
	limits.MaxTurns = 0
	if err := limits.Validate(); err == nil {
		t.Fatal("Validate() accepted zero max turns")
	}
}
