package transport

import "testing"

func TestTransportSendQueueRepresentsSixteenAudioFrames(t *testing.T) {
	if sendQueueCapacity != 16 {
		t.Fatalf("transport send queue capacity = %d, want 16", sendQueueCapacity)
	}
}
