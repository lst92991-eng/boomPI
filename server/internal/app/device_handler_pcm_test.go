package app

import (
	"bytes"
	"context"
	"encoding/json"
	"io"
	"log/slog"
	"net"
	"sync"
	"sync/atomic"
	"testing"
	"time"

	"github.com/gorilla/websocket"
	"github.com/lst92991-eng/boomPI/server/internal/backend"
	"github.com/lst92991-eng/boomPI/server/internal/config"
	"github.com/lst92991-eng/boomPI/server/internal/protocol"
)

func TestProviderAudioIsReframedWithTerminalFlagsBeforeDone(t *testing.T) {
	testCases := []struct {
		name               string
		providerPCM        [][]byte
		wantLengths        []int
		wantFlags          []uint16
		wantTerminalMarker bool
	}{
		{
			name:        "single short frame",
			providerPCM: [][]byte{pcmBoundaryFixture(100, 0)},
			wantLengths: []int{100},
			wantFlags:   []uint16{protocol.PCMFlagStart | protocol.PCMFlagEnd},
		},
		{
			name:               "exactly one 20 ms frame",
			providerPCM:        [][]byte{pcmBoundaryFixture(outputFrameBytes, 0)},
			wantLengths:        []int{outputFrameBytes, pcmBytesPerSample},
			wantFlags:          []uint16{protocol.PCMFlagStart, protocol.PCMFlagEnd},
			wantTerminalMarker: true,
		},
		{
			name: "provider chunks cross multiple wire frames",
			providerPCM: [][]byte{
				pcmBoundaryFixture(700, 0),
				pcmBoundaryFixture(260, 700),
				pcmBoundaryFixture(1042, 960),
			},
			wantLengths: []int{outputFrameBytes, outputFrameBytes, 82},
			wantFlags:   []uint16{protocol.PCMFlagStart, 0, protocol.PCMFlagEnd},
		},
		{
			name:        "one large Qwen delta is bounded on the wire",
			providerPCM: [][]byte{pcmBoundaryFixture(8*outputFrameBytes+82, 0)},
			wantLengths: []int{
				outputFrameBytes, outputFrameBytes, outputFrameBytes, outputFrameBytes,
				outputFrameBytes, outputFrameBytes, outputFrameBytes, outputFrameBytes, 82,
			},
			wantFlags: []uint16{
				protocol.PCMFlagStart, 0, 0, 0, 0, 0, 0, 0, protocol.PCMFlagEnd,
			},
		},
	}

	for _, testCase := range testCases {
		t.Run(testCase.name, func(t *testing.T) {
			events := []backend.ConversationEvent{
				{Type: backend.EventStarted, ResponseID: "response-boundary"},
			}
			for _, pcm := range testCase.providerPCM {
				events = append(events, backend.ConversationEvent{
					Type: backend.EventAudio, ResponseID: "response-boundary",
					PCM: pcm, SampleRateHz: 24_000,
				})
			}
			events = append(events, backend.ConversationEvent{
				Type: backend.EventDone, ResponseID: "response-boundary",
			})

			connection := startPCMOutputTest(t, events)
			if got := readControl(t, connection); got.Type != "response.start" {
				t.Fatalf("first response type = %q, want response.start", got.Type)
			}
			if got := readControl(t, connection); got.Type != "response.audio_start" {
				t.Fatalf("second response type = %q, want response.audio_start", got.Type)
			}

			var gotPCM []byte
			for index, wantLength := range testCase.wantLengths {
				messageType, frame, err := connection.ReadMessage()
				if err != nil {
					t.Fatalf("ReadMessage(PCM %d) error = %v", index, err)
				}
				if messageType != websocket.BinaryMessage {
					t.Fatalf("message %d type = %d, want binary PCM", index, messageType)
				}
				header, payload, err := protocol.ParsePCMFrame(frame)
				if err != nil {
					t.Fatalf("ParsePCMFrame(%d) error = %v", index, err)
				}
				if header.Kind != protocol.AudioKindDownlink ||
					header.AudioFormat != protocol.AudioFormatPCM16LE ||
					header.Channels != 1 || header.SampleRateHz != 24_000 {
					t.Fatalf("downlink format %d = %+v", index, header)
				}
				if header.Sequence != uint32(index) || header.Flags != testCase.wantFlags[index] {
					t.Fatalf("downlink ordering %d: sequence=%d flags=%#x, want sequence=%d flags=%#x",
						index, header.Sequence, header.Flags, index, testCase.wantFlags[index])
				}
				if len(payload) != wantLength {
					t.Fatalf("downlink payload %d length = %d, want %d", index, len(payload), wantLength)
				}
				gotPCM = append(gotPCM, payload...)
			}

			// Reading response.done only after all expected PCM also proves the server
			// places the terminal control after the END-marked wire frame.
			done := readControl(t, connection)
			if done.Type != "response.done" {
				t.Fatalf("message after terminal PCM = %q, want response.done", done.Type)
			}
			var wantPCM []byte
			for _, chunk := range testCase.providerPCM {
				wantPCM = append(wantPCM, chunk...)
			}
			if testCase.wantTerminalMarker {
				wantPCM = append(wantPCM, make([]byte, pcmBytesPerSample)...)
			}
			if !bytes.Equal(gotPCM, wantPCM) {
				t.Fatalf("reframed PCM length = %d, want byte-identical length %d", len(gotPCM), len(wantPCM))
			}
		})
	}
}

func TestCompleteProviderFrameDoesNotWaitForDelayedDone(t *testing.T) {
	const doneDelay = 200 * time.Millisecond
	connection := startPCMOutputTestWithBackend(t, &delayedDonePCMBackend{delay: doneDelay})
	if got := readControl(t, connection); got.Type != "response.start" {
		t.Fatalf("first response type = %q, want response.start", got.Type)
	}
	if got := readControl(t, connection); got.Type != "response.audio_start" {
		t.Fatalf("second response type = %q, want response.audio_start", got.Type)
	}

	// The provider deliberately pauses after one complete 20 ms delta. The
	// speaker frame must cross the wire during that pause, not wait for a later
	// delta or response.done to provide framing look-ahead.
	type wireMessage struct {
		messageType int
		data        []byte
		err         error
	}
	firstMessage := make(chan wireMessage, 1)
	go func() {
		messageType, data, err := connection.ReadMessage()
		firstMessage <- wireMessage{messageType: messageType, data: data, err: err}
	}()
	var first wireMessage
	select {
	case first = <-firstMessage:
	case <-time.After(doneDelay / 2):
		t.Fatal("complete 20 ms provider frame was withheld until response.done")
	}
	if first.err != nil || first.messageType != websocket.BinaryMessage {
		t.Fatalf("first media message type=%d error=%v, want binary PCM", first.messageType, first.err)
	}
	header, payload, err := protocol.ParsePCMFrame(first.data)
	if err != nil {
		t.Fatalf("ParsePCMFrame(first) error = %v", err)
	}
	if header.Sequence != 0 || header.Flags != protocol.PCMFlagStart || len(payload) != outputFrameBytes {
		t.Fatalf("first media frame sequence=%d flags=%#x bytes=%d, want sequence=0 START and %d bytes",
			header.Sequence, header.Flags, len(payload), outputFrameBytes)
	}

	messageType, frame, err := connection.ReadMessage()
	if err != nil || messageType != websocket.BinaryMessage {
		t.Fatalf("terminal marker type=%d error=%v, want binary PCM", messageType, err)
	}
	header, payload, err = protocol.ParsePCMFrame(frame)
	if err != nil {
		t.Fatalf("ParsePCMFrame(terminal) error = %v", err)
	}
	if header.Sequence != 1 || header.Flags != protocol.PCMFlagEnd || len(payload) != pcmBytesPerSample || !bytes.Equal(payload, make([]byte, pcmBytesPerSample)) {
		t.Fatalf("terminal marker sequence=%d flags=%#x payload=%v, want sequence=1 END and one silent sample",
			header.Sequence, header.Flags, payload)
	}
	if got := readControl(t, connection); got.Type != "response.done" {
		t.Fatalf("message after terminal marker = %q, want response.done", got.Type)
	}
}

func TestTextOnlyProviderResponseDoesNotInventPCMEndMarker(t *testing.T) {
	connection := startPCMOutputTest(t, []backend.ConversationEvent{
		{Type: backend.EventStarted, ResponseID: "response-text-only"},
		{Type: backend.EventTextDelta, ResponseID: "response-text-only", Text: "hello"},
		{Type: backend.EventDone, ResponseID: "response-text-only"},
	})
	for _, want := range []string{"response.start", "response.text_delta", "response.done"} {
		if got := readControl(t, connection); got.Type != want {
			t.Fatalf("control type = %q, want %q; text-only response must not synthesize PCM", got.Type, want)
		}
	}
}

func TestProviderAudioRejectsInvalidPCM(t *testing.T) {
	testCases := []struct {
		name         string
		pcm          []byte
		sampleRateHz int
	}{
		{name: "odd S16 LE byte count", pcm: []byte{1, 2, 3}, sampleRateHz: 24_000},
		{name: "wrong sample rate", pcm: []byte{1, 2, 3, 4}, sampleRateHz: 16_000},
	}

	for _, testCase := range testCases {
		t.Run(testCase.name, func(t *testing.T) {
			connection := startPCMOutputTest(t, []backend.ConversationEvent{
				{Type: backend.EventStarted, ResponseID: "response-invalid"},
				{Type: backend.EventAudio, ResponseID: "response-invalid", PCM: testCase.pcm, SampleRateHz: testCase.sampleRateHz},
				{Type: backend.EventDone, ResponseID: "response-invalid"},
			})
			if got := readControl(t, connection); got.Type != "response.start" {
				t.Fatalf("first response type = %q, want response.start", got.Type)
			}
			if messageType, data, err := connection.ReadMessage(); err == nil {
				t.Fatalf("invalid provider PCM produced WebSocket message type=%d data=%q, want connection rejection", messageType, data)
			}
		})
	}
}

func TestProviderAudioValidationUsesBoundedProviderDelta(t *testing.T) {
	pcm := make([]byte, maxProviderAudioDeltaBytes)
	if err := validateProviderAudio(outputSampleRateHz, pcm); err != nil {
		t.Fatalf("maximum provider delta validation error = %v", err)
	}
	oversized := make([]byte, maxProviderAudioDeltaBytes+pcmBytesPerSample)
	if err := validateProviderAudio(outputSampleRateHz, oversized); err == nil {
		t.Fatal("oversized provider delta unexpectedly succeeded")
	}
}

func TestBurstProviderAudioIsPacedAtSpeakerRate(t *testing.T) {
	const frameCount = 60
	events := []backend.ConversationEvent{
		{Type: backend.EventStarted, ResponseID: "response-paced"},
		{
			Type:         backend.EventAudio,
			ResponseID:   "response-paced",
			PCM:          pcmBoundaryFixture(frameCount*outputFrameBytes, 0),
			SampleRateHz: outputSampleRateHz,
		},
		{Type: backend.EventDone, ResponseID: "response-paced"},
	}
	connection := startPCMOutputTest(t, events)
	if got := readControl(t, connection); got.Type != "response.start" {
		t.Fatalf("first response type = %q, want response.start", got.Type)
	}
	if got := readControl(t, connection); got.Type != "response.audio_start" {
		t.Fatalf("second response type = %q, want response.audio_start", got.Type)
	}
	if err := connection.SetReadDeadline(time.Now().Add(3 * time.Second)); err != nil {
		t.Fatalf("SetReadDeadline() error = %v", err)
	}

	var firstAt time.Time
	var lastAt time.Time
	var previousTimestampUS uint64
	for index := 0; index < frameCount; index++ {
		messageType, frame, err := connection.ReadMessage()
		if err != nil {
			t.Fatalf("ReadMessage(PCM %d) error = %v", index, err)
		}
		if messageType != websocket.BinaryMessage {
			t.Fatalf("message %d type = %d, want binary PCM", index, messageType)
		}
		header, payload, err := protocol.ParsePCMFrame(frame)
		if err != nil {
			t.Fatalf("ParsePCMFrame(%d) error = %v", index, err)
		}
		wantFlags := uint16(0)
		if index == 0 {
			wantFlags |= protocol.PCMFlagStart
		}
		if index+1 == frameCount {
			wantFlags |= protocol.PCMFlagEnd
		}
		if len(payload) != outputFrameBytes || header.Sequence != uint32(index) ||
			header.Flags != wantFlags {
			t.Fatalf("paced frame %d: sequence=%d flags=%#x bytes=%d, want sequence=%d flags=%#x bytes=%d",
				index, header.Sequence, header.Flags, len(payload), index, wantFlags, outputFrameBytes)
		}
		if index != 0 {
			if header.TimestampUS <= previousTimestampUS {
				t.Fatalf("paced timestamp %d = %d, previous=%d", index, header.TimestampUS, previousTimestampUS)
			}
			// The production floor is 15 ms.  Keep 1 ms for timestamp and
			// scheduler measurement noise while still rejecting the 5-10 ms
			// catch-up bursts that caused periodic playback stalls.
			if delta := header.TimestampUS - previousTimestampUS; delta < 14_000 {
				t.Fatalf("paced timestamp delta %d = %d us, want at least 14000 us", index, delta)
			}
		}
		previousTimestampUS = header.TimestampUS
		now := time.Now()
		if index == 0 {
			firstAt = now
		}
		lastAt = now
	}
	span := lastAt.Sub(firstAt)
	if span < time.Second {
		t.Fatalf("%d provider frames arrived in %s, want real-time pacing", frameCount, span)
	}
	if span > 2500*time.Millisecond {
		t.Fatalf("%d paced frames took %s, want less than 2.5s", frameCount, span)
	}
	if got := readControl(t, connection); got.Type != "response.done" {
		t.Fatalf("message after paced PCM = %q, want response.done", got.Type)
	}
}

func TestNextPacedFrameDeadlineBoundsCatchUp(t *testing.T) {
	base := time.Unix(100, 0)
	for _, test := range []struct {
		name     string
		previous time.Time
		sentAt   time.Time
		want     time.Time
	}{
		{name: "first frame", sentAt: base, want: base.Add(20 * time.Millisecond)},
		{name: "on cadence", previous: base, sentAt: base, want: base.Add(20 * time.Millisecond)},
		{name: "small overshoot", previous: base, sentAt: base.Add(4 * time.Millisecond), want: base.Add(20 * time.Millisecond)},
		{name: "catch-up floor", previous: base, sentAt: base.Add(12 * time.Millisecond), want: base.Add(27 * time.Millisecond)},
		{name: "full frame stall", previous: base, sentAt: base.Add(20 * time.Millisecond), want: base.Add(40 * time.Millisecond)},
	} {
		t.Run(test.name, func(t *testing.T) {
			if got := nextPacedFrameDeadline(test.previous, test.sentAt); !got.Equal(test.want) {
				t.Fatalf("next deadline = %s, want %s", got, test.want)
			}
		})
	}
}

func TestActiveResponseCancelPreemptsPacedAudio(t *testing.T) {
	const (
		frameCount       = 60
		framesBeforeStop = 8
	)
	var providerCancels atomic.Int32
	connection := startPCMOutputTestWithCancelCounter(t, []backend.ConversationEvent{
		{Type: backend.EventStarted, ResponseID: "response-cancel-paced"},
		{
			Type:         backend.EventAudio,
			ResponseID:   "response-cancel-paced",
			PCM:          pcmBoundaryFixture(frameCount*outputFrameBytes, 0),
			SampleRateHz: outputSampleRateHz,
		},
	}, &providerCancels)
	started := readControl(t, connection)
	if started.Type != "response.start" || started.StreamID == 0 {
		t.Fatalf("first response = %+v, want response.start with stream", started)
	}
	if got := readControl(t, connection); got.Type != "response.audio_start" {
		t.Fatalf("second response type = %q, want response.audio_start", got.Type)
	}

	for index := 0; index < framesBeforeStop; index++ {
		messageType, frame, err := connection.ReadMessage()
		if err != nil {
			t.Fatalf("ReadMessage(PCM %d) error = %v", index, err)
		}
		if messageType != websocket.BinaryMessage {
			t.Fatalf("message %d type = %d, want binary PCM", index, messageType)
		}
		header, _, err := protocol.ParsePCMFrame(frame)
		if err != nil {
			t.Fatalf("ParsePCMFrame(%d) error = %v", index, err)
		}
		if header.Sequence != uint32(index) {
			t.Fatalf("paced frame %d sequence=%d", index, header.Sequence)
		}
	}

	cancelAt := time.Now()
	writeControl(t, connection, protocol.ControlEnvelope{
		Version: protocol.Version, Type: "response.cancel", MessageID: "client-cancel-paced",
		DeviceID: testDeviceID, SessionID: started.SessionID, TurnID: started.TurnID,
		StreamID: started.StreamID, Epoch: started.Epoch, Payload: json.RawMessage(`{}`),
	})
	if err := connection.SetReadDeadline(cancelAt.Add(250 * time.Millisecond)); err != nil {
		t.Fatalf("SetReadDeadline(cancel) error = %v", err)
	}
	pcmBeforeAck := 0
	for {
		messageType, data, err := connection.ReadMessage()
		if err != nil {
			t.Fatalf("wait for response.cancelled: %v", err)
		}
		if messageType == websocket.BinaryMessage {
			pcmBeforeAck++
			continue
		}
		if messageType != websocket.TextMessage {
			t.Fatalf("message before cancellation ack type=%d", messageType)
		}
		ack, err := protocol.DecodeControl(data)
		if err != nil {
			t.Fatalf("DecodeControl(cancel ack) error = %v", err)
		}
		if ack.Type != "response.cancelled" {
			t.Fatalf("control before cancellation ack = %q", ack.Type)
		}
		if ack.SessionID != started.SessionID || ack.TurnID != started.TurnID ||
			ack.StreamID != started.StreamID || ack.Epoch != started.Epoch {
			t.Fatalf("response.cancelled identity = %+v, want active response", ack)
		}
		break
	}
	if elapsed := time.Since(cancelAt); elapsed > 250*time.Millisecond {
		t.Fatalf("response.cancelled arrived after %s, want at most 250ms", elapsed)
	}
	if pcmBeforeAck > 1 {
		t.Fatalf("received %d paced PCM frames before cancel ack, want at most 1", pcmBeforeAck)
	}
	if got := providerCancels.Load(); got != 1 {
		t.Fatalf("provider Cancel calls = %d, want 1", got)
	}

	if err := connection.SetReadDeadline(time.Now().Add(100 * time.Millisecond)); err != nil {
		t.Fatalf("SetReadDeadline(post-cancel) error = %v", err)
	}
	if messageType, data, err := connection.ReadMessage(); err == nil {
		t.Fatalf("retired response produced message after cancel ack: type=%d bytes=%d", messageType, len(data))
	} else if networkError, ok := err.(net.Error); !ok || !networkError.Timeout() {
		t.Fatalf("connection failed after cancel ack instead of remaining idle: %v", err)
	}
}

func TestValidateUplinkHeaderRejectsTurnMismatches(t *testing.T) {
	device, err := protocol.ParseDeviceUUID(testDeviceID)
	if err != nil {
		t.Fatalf("ParseDeviceUUID() error = %v", err)
	}
	turn := activeTurn{
		deviceUUID: device, sessionID: 8, turnID: 9, uplinkStream: 10,
		epoch: 7, expectedInput: 42,
	}
	header := protocol.PCMHeader{
		Kind: protocol.AudioKindUplink, SampleRateHz: 16_000, DeviceUUID: device,
		SessionID: 8, TurnID: 9, StreamID: 10, Epoch: 7, Sequence: 42,
	}
	if err := validateUplinkHeader(turn, header); err != nil {
		t.Fatalf("validateUplinkHeader() valid header error = %v", err)
	}
	for name, mutate := range map[string]func(*protocol.PCMHeader){
		"epoch":       func(value *protocol.PCMHeader) { value.Epoch++ },
		"sample_rate": func(value *protocol.PCMHeader) { value.SampleRateHz = 24_000 },
		"sequence":    func(value *protocol.PCMHeader) { value.Sequence++ },
	} {
		t.Run(name, func(t *testing.T) {
			candidate := header
			mutate(&candidate)
			if err := validateUplinkHeader(turn, candidate); err == nil {
				t.Fatal("validateUplinkHeader() accepted a turn mismatch")
			}
		})
	}
}

func TestUplinkFramingStateMachine(t *testing.T) {
	t.Run("multi-frame happy path then rejects PCM after END", func(t *testing.T) {
		turn := activeTurn{}
		if err := validateAndAdvanceUplinkFraming(&turn, protocol.PCMFlagStart); err != nil {
			t.Fatalf("START frame error = %v", err)
		}
		if err := validateAndAdvanceUplinkFraming(&turn, 0); err != nil {
			t.Fatalf("middle frame error = %v", err)
		}
		if err := validateAndAdvanceUplinkFraming(&turn, protocol.PCMFlagEnd); err != nil {
			t.Fatalf("END frame error = %v", err)
		}
		if err := validateUplinkCommit(turn); err != nil {
			t.Fatalf("commit validation error = %v", err)
		}
		if err := validateAndAdvanceUplinkFraming(&turn, 0); err == nil {
			t.Fatal("PCM after END unexpectedly succeeded")
		}
	})

	t.Run("single frame may carry START and END", func(t *testing.T) {
		turn := activeTurn{}
		if err := validateAndAdvanceUplinkFraming(
			&turn, protocol.PCMFlagStart|protocol.PCMFlagEnd); err != nil {
			t.Fatalf("single-frame turn error = %v", err)
		}
		if err := validateUplinkCommit(turn); err != nil {
			t.Fatalf("single-frame commit validation error = %v", err)
		}
	})

	testCases := []struct {
		name  string
		turn  activeTurn
		flags uint16
	}{
		{name: "first frame without START", flags: 0},
		{name: "duplicate START", turn: activeTurn{inputStarted: true}, flags: protocol.PCMFlagStart},
		{name: "discontinuity", flags: protocol.PCMFlagStart | protocol.PCMFlagDiscontinuity},
		{name: "PCM after commit", turn: activeTurn{inputStarted: true, inputEnded: true, inputCommitted: true}},
	}
	for _, testCase := range testCases {
		t.Run(testCase.name, func(t *testing.T) {
			turn := testCase.turn
			if err := validateAndAdvanceUplinkFraming(&turn, testCase.flags); err == nil {
				t.Fatal("invalid uplink framing unexpectedly succeeded")
			}
		})
	}
}

func TestUplinkCommitRequiresTerminalFrame(t *testing.T) {
	testCases := []struct {
		name string
		turn activeTurn
	}{
		{name: "no PCM"},
		{name: "START without END", turn: activeTurn{inputStarted: true}},
		{name: "duplicate commit", turn: activeTurn{inputStarted: true, inputEnded: true, inputCommitted: true}},
	}
	for _, testCase := range testCases {
		t.Run(testCase.name, func(t *testing.T) {
			if err := validateUplinkCommit(testCase.turn); err == nil {
				t.Fatal("invalid turn.commit ordering unexpectedly succeeded")
			}
		})
	}
}

func pcmBoundaryFixture(length int, offset int) []byte {
	pcm := make([]byte, length)
	for index := range pcm {
		pcm[index] = byte((offset + index) % 251)
	}
	return pcm
}

func startPCMOutputTest(t *testing.T, events []backend.ConversationEvent) *websocket.Conn {
	return startPCMOutputTestWithCancelCounter(t, events, nil)
}

func startPCMOutputTestWithCancelCounter(t *testing.T, events []backend.ConversationEvent, cancelCount *atomic.Int32) *websocket.Conn {
	return startPCMOutputTestWithBackend(t, &pcmBoundaryBackend{
		events: events, cancelCount: cancelCount,
	})
}

func startPCMOutputTestWithBackend(t *testing.T, provider backend.ConversationBackend) *websocket.Conn {
	t.Helper()
	t.Setenv("DASHSCOPE_API_KEY", "offline-test-key")
	t.Setenv("DASHSCOPE_WORKSPACE_ID", "offline-test-workspace")
	cfg, err := config.Load("")
	if err != nil {
		t.Fatalf("config.Load() error = %v", err)
	}
	cfg.ListenAddress = "127.0.0.1"
	cfg.WSSPort = freePort(t)
	cfg.DiscoveryPort = freeUDPPort(t)
	logger := slog.New(slog.NewTextHandler(io.Discard, nil))
	application, err := newWithBackend(cfg, logger, t.TempDir(), provider)
	if err != nil {
		t.Fatalf("newWithBackend() error = %v", err)
	}

	ctx, cancel := context.WithCancel(context.Background())
	serverDone := make(chan error, 1)
	go func() { serverDone <- application.Run(ctx) }()
	connection := dialTestServer(t, cfg.WSSPort)
	t.Cleanup(func() {
		_ = connection.Close()
		cancel()
		select {
		case err := <-serverDone:
			if err != nil {
				t.Errorf("App.Run() error = %v", err)
			}
		case <-time.After(2 * time.Second):
			t.Error("server did not stop")
		}
	})

	writeControl(t, connection, protocol.ControlEnvelope{
		Version: protocol.Version, Type: "hello", MessageID: "client-boundary-1", DeviceID: testDeviceID,
		Payload: json.RawMessage(`{"device_token":"` + testDeviceToken + `"}`),
	})
	helloAck := readControl(t, connection)
	if helloAck.Type != "hello.ack" || helloAck.SessionID == 0 {
		t.Fatalf("hello.ack = %+v", helloAck)
	}
	turn := protocol.ControlEnvelope{
		Version: protocol.Version, Type: "turn.start", MessageID: "client-boundary-2", DeviceID: testDeviceID,
		SessionID: helloAck.SessionID, TurnID: 41, StreamID: 42, Epoch: 1,
		Payload: json.RawMessage(`{"sample_rate_hz":16000}`),
	}
	writeControl(t, connection, turn)
	deviceUUID, err := protocol.ParseDeviceUUID(testDeviceID)
	if err != nil {
		t.Fatalf("ParseDeviceUUID() error = %v", err)
	}
	uplink := make([]byte, inputFrameBytes)
	writePCM(t, connection, protocol.PCMHeader{
		Version: protocol.Version, Kind: protocol.AudioKindUplink,
		Flags:       protocol.PCMFlagStart | protocol.PCMFlagEnd,
		AudioFormat: protocol.AudioFormatPCM16LE, Channels: 1, SampleRateHz: 16_000,
		PayloadLen: uint32(len(uplink)), Sequence: 0, Epoch: 1, DeviceUUID: deviceUUID,
		SessionID: helloAck.SessionID, TurnID: 41, StreamID: 42,
	}, uplink)
	turn.Type = "turn.commit"
	turn.MessageID = "client-boundary-3"
	turn.Payload = json.RawMessage(`{}`)
	writeControl(t, connection, turn)
	return connection
}

type delayedDonePCMBackend struct {
	delay time.Duration
}

func (b *delayedDonePCMBackend) Open(context.Context, backend.SessionConfig) (backend.ConversationSession, error) {
	return &delayedDonePCMSession{
		delay:  b.delay,
		events: make(chan backend.ConversationEvent, 3),
		stop:   make(chan struct{}),
	}, nil
}

type delayedDonePCMSession struct {
	delay     time.Duration
	events    chan backend.ConversationEvent
	stop      chan struct{}
	workers   sync.WaitGroup
	closeOnce sync.Once
}

func (*delayedDonePCMSession) SendAudio(context.Context, []byte) error { return nil }

func (s *delayedDonePCMSession) Commit(context.Context) error {
	s.workers.Add(1)
	go func() {
		defer s.workers.Done()
		if !s.emit(backend.ConversationEvent{Type: backend.EventStarted, ResponseID: "response-delayed-done"}) {
			return
		}
		if !s.emit(backend.ConversationEvent{
			Type: backend.EventAudio, ResponseID: "response-delayed-done",
			PCM: pcmBoundaryFixture(outputFrameBytes, 0), SampleRateHz: outputSampleRateHz,
		}) {
			return
		}
		timer := time.NewTimer(s.delay)
		defer timer.Stop()
		select {
		case <-s.stop:
			return
		case <-timer.C:
		}
		s.emit(backend.ConversationEvent{Type: backend.EventDone, ResponseID: "response-delayed-done"})
	}()
	return nil
}

func (*delayedDonePCMSession) Cancel(context.Context) error { return nil }

func (s *delayedDonePCMSession) Events() <-chan backend.ConversationEvent { return s.events }

func (s *delayedDonePCMSession) emit(event backend.ConversationEvent) bool {
	select {
	case <-s.stop:
		return false
	case s.events <- event:
		return true
	}
}

func (s *delayedDonePCMSession) Close() error {
	s.closeOnce.Do(func() {
		close(s.stop)
		s.workers.Wait()
		close(s.events)
	})
	return nil
}

type pcmBoundaryBackend struct {
	events      []backend.ConversationEvent
	cancelCount *atomic.Int32
}

func (b *pcmBoundaryBackend) Open(context.Context, backend.SessionConfig) (backend.ConversationSession, error) {
	return &pcmBoundarySession{
		eventsToSend: append([]backend.ConversationEvent(nil), b.events...),
		events:       make(chan backend.ConversationEvent, len(b.events)+1),
		cancelCount:  b.cancelCount,
	}, nil
}

type pcmBoundarySession struct {
	eventsToSend []backend.ConversationEvent
	events       chan backend.ConversationEvent
	cancelCount  *atomic.Int32
	closeOnce    sync.Once
}

func (*pcmBoundarySession) SendAudio(context.Context, []byte) error { return nil }

func (s *pcmBoundarySession) Commit(context.Context) error {
	for _, event := range s.eventsToSend {
		s.events <- event
	}
	return nil
}

func (s *pcmBoundarySession) Cancel(context.Context) error {
	if s.cancelCount != nil {
		s.cancelCount.Add(1)
	}
	return nil
}

func (s *pcmBoundarySession) Events() <-chan backend.ConversationEvent { return s.events }

func (s *pcmBoundarySession) Close() error {
	s.closeOnce.Do(func() { close(s.events) })
	return nil
}
