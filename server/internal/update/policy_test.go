package update

import "testing"

func TestPolicyValidate(t *testing.T) {
	policy := DefaultPolicy()
	if err := policy.Validate(); err != nil {
		t.Fatalf("Validate() error = %v", err)
	}
	unsigned := policy
	unsigned.RequireSignature = false
	if err := unsigned.Validate(); err == nil {
		t.Fatal("Validate() accepted unsigned updates")
	}
	unbounded := policy
	unbounded.MaxPackageBytes = 0
	if err := unbounded.Validate(); err == nil {
		t.Fatal("Validate() accepted an unbounded package size")
	}
}
