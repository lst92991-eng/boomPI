package discovery

import "testing"

func TestConfigValidate(t *testing.T) {
	valid := Config{BindAddress: "0.0.0.0", UDPPort: 17807, WSSPort: 17806}
	if err := valid.Validate(); err != nil {
		t.Fatalf("Validate() error = %v", err)
	}
	invalidAddress := valid
	invalidAddress.BindAddress = "localhost"
	if err := invalidAddress.Validate(); err == nil {
		t.Fatal("Validate() accepted a hostname")
	}
	samePorts := valid
	samePorts.UDPPort = samePorts.WSSPort
	if err := samePorts.Validate(); err == nil {
		t.Fatal("Validate() accepted duplicate ports")
	}
}
