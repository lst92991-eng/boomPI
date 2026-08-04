package session

import "testing"

func TestActorEventQueueRepresentsEightAudioFrames(t *testing.T) {
	if eventCapacity != 8 {
		t.Fatalf("actor event queue capacity = %d, want 8", eventCapacity)
	}
}
