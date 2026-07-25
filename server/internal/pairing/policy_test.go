package pairing

import "testing"

func TestPolicyValidate(t *testing.T) {
	policy := DefaultPolicy()
	if err := policy.Validate(); err != nil {
		t.Fatalf("Validate() error = %v", err)
	}
	policy.CodeDigits = 5
	if err := policy.Validate(); err == nil {
		t.Fatal("Validate() accepted a non-six-digit code")
	}
}
