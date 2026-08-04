package app

import "testing"

func TestProductionProviderQueueRepresentsEightAudioFrames(t *testing.T) {
	if providerEventQueueCapacity != 8 {
		t.Fatalf("provider event queue capacity = %d, want 8", providerEventQueueCapacity)
	}
}
