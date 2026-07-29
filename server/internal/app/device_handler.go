package app

import (
	"bytes"
	"context"
	"crypto/rand"
	"crypto/sha256"
	"crypto/subtle"
	"encoding/binary"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"log/slog"
	"sync"
	"sync/atomic"
	"time"

	"github.com/lst92991-eng/boomPI/server/internal/backend"
	"github.com/lst92991-eng/boomPI/server/internal/config"
	"github.com/lst92991-eng/boomPI/server/internal/protocol"
	"github.com/lst92991-eng/boomPI/server/internal/session"
	"github.com/lst92991-eng/boomPI/server/internal/transport"
)

const (
	inputFrameBytes            = 640
	outputSampleRateHz         = 24_000
	pcmBytesPerSample          = 2
	outputFrameDurationMS      = 20
	outputFrameBytes           = outputSampleRateHz * pcmBytesPerSample * outputFrameDurationMS / 1000
	helloAuthenticationTimeout = 5 * time.Second
)

var errDeviceAuthentication = errors.New("device authentication failed")

type deviceHandler struct {
	cfg      config.Config
	logger   *slog.Logger
	provider backend.ConversationBackend
}

type activeTurn struct {
	deviceID       string
	deviceUUID     [16]byte
	sessionID      uint32
	turnID         uint32
	uplinkStream   uint32
	downlinkStream uint32
	epoch          uint32
	expectedInput  uint32
	inputStarted   bool
	inputEnded     bool
	inputCommitted bool
	outputSequence uint32
	pendingPCM     []byte
	pendingOutput  []byte
	outputStarted  bool
	active         bool
}

type connectionState struct {
	mu             sync.Mutex
	turn           activeTurn
	message        atomic.Uint64
	monotonicStart time.Time
}

func (h *deviceHandler) Handle(ctx context.Context, connection *transport.Connection) error {
	helloCtx, cancelHello := context.WithTimeout(ctx, helloAuthenticationTimeout)
	first, err := connection.Receive(helloCtx)
	cancelHello()
	if err != nil {
		return fmt.Errorf("receive device hello: %w", err)
	}
	if first.Control == nil || first.Control.Type != "hello" {
		return errors.New("first device message must be hello")
	}
	hello := *first.Control
	if hello.SessionID != 0 || hello.TurnID != 0 || hello.StreamID != 0 || hello.Epoch != 0 {
		return errors.New("hello identifiers must be zero")
	}
	if err := authenticateHello(hello.Payload, h.cfg.DeviceToken.Value()); err != nil {
		return err
	}
	deviceUUID, err := protocol.ParseDeviceUUID(hello.DeviceID)
	if err != nil {
		return err
	}
	sessionID, err := randomNonzeroID()
	if err != nil {
		return err
	}
	actor, err := session.Open(ctx, h.provider, backend.SessionConfig{
		DeviceID: hello.DeviceID, SystemPrompt: h.cfg.SystemPrompt, Persona: h.cfg.Persona,
	})
	if err != nil {
		return err
	}
	defer actor.Close()

	state := &connectionState{
		turn:           activeTurn{deviceID: hello.DeviceID, deviceUUID: deviceUUID, sessionID: sessionID, epoch: 1},
		monotonicStart: time.Now(),
	}
	if err := sendControl(ctx, connection, state, "hello.ack", state.turn, map[string]any{
		"input_sample_rate_hz":  16_000,
		"output_sample_rate_hz": 24_000,
		"input_frame_ms":        20,
	}); err != nil {
		return err
	}
	deviceRef := redactedDeviceRef(hello.DeviceID)
	h.logger.Info("device connected", "device_ref", deviceRef)
	defer h.logger.Info("device disconnected", "device_ref", deviceRef)

	workerCtx, stopWorker := context.WithCancelCause(ctx)
	workerDone := make(chan error, 1)
	go func() {
		err := h.forwardEvents(workerCtx, connection, actor, state)
		workerDone <- err
		stopWorker(err)
	}()
	defer func() {
		stopWorker(context.Canceled)
		<-workerDone
	}()

	for {
		message, receiveErr := connection.Receive(workerCtx)
		if receiveErr != nil {
			return receiveErr
		}
		if message.Control != nil {
			if err := h.handleControl(workerCtx, connection, actor, state, *message.Control); err != nil {
				_ = sendProtocolError(workerCtx, connection, state, err)
				return err
			}
			continue
		}
		if err := h.handlePCM(workerCtx, actor, state, *message.PCMHeader, message.PCM); err != nil {
			_ = sendProtocolError(workerCtx, connection, state, err)
			return err
		}
	}
}

func authenticateHello(payload json.RawMessage, expectedToken string) error {
	presentedToken, err := parseHelloDeviceToken(payload)
	if err != nil {
		return errDeviceAuthentication
	}
	presentedDigest := sha256.Sum256([]byte(presentedToken))
	expectedDigest := sha256.Sum256([]byte(expectedToken))
	if subtle.ConstantTimeCompare(presentedDigest[:], expectedDigest[:]) != 1 {
		return errDeviceAuthentication
	}
	return nil
}

func parseHelloDeviceToken(payload json.RawMessage) (string, error) {
	decoder := json.NewDecoder(bytes.NewReader(payload))
	opening, err := decoder.Token()
	if err != nil || opening != json.Delim('{') {
		return "", errDeviceAuthentication
	}
	if !decoder.More() {
		return "", errDeviceAuthentication
	}
	key, err := decoder.Token()
	if err != nil || key != "device_token" {
		return "", errDeviceAuthentication
	}
	var token string
	if err := decoder.Decode(&token); err != nil || token == "" {
		return "", errDeviceAuthentication
	}
	if decoder.More() {
		return "", errDeviceAuthentication
	}
	closing, err := decoder.Token()
	if err != nil || closing != json.Delim('}') {
		return "", errDeviceAuthentication
	}
	if _, err := decoder.Token(); !errors.Is(err, io.EOF) {
		return "", errDeviceAuthentication
	}
	return token, nil
}

func (h *deviceHandler) handleControl(ctx context.Context, connection *transport.Connection, actor *session.Actor, state *connectionState, envelope protocol.ControlEnvelope) error {
	state.mu.Lock()
	turn := state.turn
	state.mu.Unlock()
	if envelope.DeviceID != turn.deviceID || envelope.SessionID != turn.sessionID {
		return errors.New("control message does not belong to this device session")
	}

	switch envelope.Type {
	case "turn.start":
		if turn.active {
			return errors.New("a turn is already active")
		}
		if envelope.TurnID == 0 || envelope.StreamID == 0 || envelope.Epoch == 0 {
			return errors.New("turn.start identifiers must be nonzero")
		}
		var payload struct {
			SampleRateHz uint32 `json:"sample_rate_hz"`
		}
		if err := json.Unmarshal(envelope.Payload, &payload); err != nil || payload.SampleRateHz != 16_000 {
			return errors.New("turn.start requires sample_rate_hz 16000")
		}
		downlinkStream, err := randomNonzeroID()
		if err != nil {
			return err
		}
		if err := actor.StartTurn(ctx, uint64(envelope.Epoch)); err != nil {
			return err
		}
		turn.turnID = envelope.TurnID
		turn.uplinkStream = envelope.StreamID
		turn.downlinkStream = downlinkStream
		turn.epoch = envelope.Epoch
		turn.expectedInput = 0
		turn.inputStarted = false
		turn.inputEnded = false
		turn.inputCommitted = false
		turn.outputSequence = 0
		turn.pendingPCM = nil
		turn.pendingOutput = nil
		turn.outputStarted = false
		turn.active = true
		state.mu.Lock()
		state.turn = turn
		state.mu.Unlock()
		return nil

	case "turn.commit":
		if err := validateActiveEnvelope(turn, envelope); err != nil {
			return err
		}
		if err := validateUplinkCommit(turn); err != nil {
			return err
		}
		if err := flushFinalInput(ctx, actor, &turn); err != nil {
			return err
		}
		turn.inputCommitted = true
		state.mu.Lock()
		state.turn = turn
		state.mu.Unlock()
		return actor.Commit(ctx, uint64(turn.epoch))

	case "turn.cancel", "response.cancel":
		if !turn.active {
			if envelope.TurnID != turn.turnID || envelope.Epoch != turn.epoch ||
				(envelope.StreamID != turn.uplinkStream && envelope.StreamID != turn.downlinkStream) {
				return nil
			}
			return sendControl(ctx, connection, state, "response.cancelled", turn, map[string]any{"reason": "client_request"})
		}
		if envelope.Epoch != turn.epoch {
			return errors.New("cancel epoch does not match the active turn")
		}
		// Fence downlink immediately. Provider cancellation may wait for a remote
		// acknowledgement, but no event from this epoch should be queued meanwhile.
		state.mu.Lock()
		if sameTurn(state.turn, turn) {
			state.turn.active = false
			state.turn.pendingPCM = nil
			state.turn.pendingOutput = nil
		}
		state.mu.Unlock()
		if err := actor.Cancel(ctx, uint64(turn.epoch)); err != nil {
			return err
		}
		turn.active = false
		turn.pendingPCM = nil
		turn.pendingOutput = nil
		return sendControl(ctx, connection, state, "response.cancelled", turn, map[string]any{"reason": "client_request"})
	default:
		return fmt.Errorf("unsupported control type %q", envelope.Type)
	}
}

func (h *deviceHandler) handlePCM(ctx context.Context, actor *session.Actor, state *connectionState, header protocol.PCMHeader, pcm []byte) error {
	state.mu.Lock()
	turn := &state.turn
	if !turn.active {
		state.mu.Unlock()
		return errors.New("PCM arrived without an active turn")
	}
	stream := protocol.PCMStreamContext{
		Kind: protocol.AudioKindUplink, SampleRateHz: 16_000, DeviceUUID: turn.deviceUUID,
		Epoch: turn.epoch, SessionID: turn.sessionID, TurnID: turn.turnID,
		StreamID: turn.uplinkStream, ExpectedSequence: turn.expectedInput,
	}
	if err := stream.ValidateHeader(header); err != nil {
		state.mu.Unlock()
		return err
	}
	if err := validateAndAdvanceUplinkFraming(turn, header.Flags); err != nil {
		state.mu.Unlock()
		return err
	}
	turn.expectedInput++
	turn.pendingPCM = append(turn.pendingPCM, pcm...)
	completeBytes := len(turn.pendingPCM) / inputFrameBytes * inputFrameBytes
	completePCM := append([]byte(nil), turn.pendingPCM[:completeBytes]...)
	turn.pendingPCM = append(turn.pendingPCM[:0], turn.pendingPCM[completeBytes:]...)
	epoch := turn.epoch
	state.mu.Unlock()

	// Provider backpressure must not hold connectionState.mu: the event forwarder
	// needs that lock to deliver text/audio or retire a completed turn.
	for offset := 0; offset < len(completePCM); offset += inputFrameBytes {
		if err := actor.AppendPCM(ctx, uint64(epoch), completePCM[offset:offset+inputFrameBytes]); err != nil {
			return err
		}
	}
	return nil
}

func validateAndAdvanceUplinkFraming(turn *activeTurn, flags uint16) error {
	if turn.inputCommitted {
		return errors.New("PCM arrived after turn.commit")
	}
	if turn.inputEnded {
		return errors.New("PCM arrived after the uplink END marker")
	}
	if flags&protocol.PCMFlagDiscontinuity != 0 {
		return errors.New("uplink PCM discontinuity cancels the active turn")
	}
	start := flags&protocol.PCMFlagStart != 0
	if !turn.inputStarted {
		if !start {
			return errors.New("first uplink PCM frame must carry START")
		}
		turn.inputStarted = true
	} else if start {
		return errors.New("uplink PCM START marker may only appear on the first frame")
	}
	if flags&protocol.PCMFlagEnd != 0 {
		turn.inputEnded = true
	}
	return nil
}

func validateUplinkCommit(turn activeTurn) error {
	if turn.inputCommitted {
		return errors.New("turn.commit was received more than once")
	}
	if !turn.inputStarted {
		return errors.New("turn.commit requires at least one uplink PCM frame")
	}
	if !turn.inputEnded {
		return errors.New("turn.commit requires the uplink END marker")
	}
	return nil
}

func flushFinalInput(ctx context.Context, actor *session.Actor, turn *activeTurn) error {
	if len(turn.pendingPCM) == 0 {
		return nil
	}
	frame := make([]byte, inputFrameBytes)
	copy(frame, turn.pendingPCM)
	turn.pendingPCM = nil
	return actor.AppendPCM(ctx, uint64(turn.epoch), frame)
}

func (h *deviceHandler) forwardEvents(ctx context.Context, connection *transport.Connection, actor *session.Actor, state *connectionState) error {
	for {
		select {
		case <-ctx.Done():
			return nil
		case event, ok := <-actor.Events():
			if !ok {
				return nil
			}
			state.mu.Lock()
			turn := state.turn
			state.mu.Unlock()
			if !turn.active || event.Epoch != uint64(turn.epoch) {
				continue
			}
			switch event.Type {
			case backend.EventStarted:
				if err := sendControlIfActive(ctx, connection, state, "response.start", turn, map[string]any{"response_id": event.ResponseID}); err != nil {
					return err
				}
			case backend.EventTextDelta:
				if err := sendControlIfActive(ctx, connection, state, "response.text_delta", turn, map[string]any{"response_id": event.ResponseID, "text": event.Text}); err != nil {
					return err
				}
			case backend.EventAudio:
				if err := validateProviderAudio(event.SampleRateHz, event.PCM); err != nil {
					return err
				}
				state.mu.Lock()
				if !sameActiveTurn(state.turn, turn) {
					state.mu.Unlock()
					continue
				}
				turn = state.turn
				if !turn.outputStarted {
					if err := sendControl(ctx, connection, state, "response.audio_start", turn, map[string]any{"response_id": event.ResponseID, "sample_rate_hz": event.SampleRateHz}); err != nil {
						state.mu.Unlock()
						return err
					}
					turn.outputStarted = true
				}
				turn.pendingOutput = append(turn.pendingOutput, event.PCM...)
				for len(turn.pendingOutput) > outputFrameBytes {
					payload := append([]byte(nil), turn.pendingOutput[:outputFrameBytes]...)
					copy(turn.pendingOutput, turn.pendingOutput[outputFrameBytes:])
					turn.pendingOutput = turn.pendingOutput[:len(turn.pendingOutput)-outputFrameBytes]
					if err := sendDownlinkPCM(ctx, connection, state, turn, payload, false); err != nil {
						state.mu.Unlock()
						return err
					}
					turn.outputSequence++
				}
				state.turn = turn
				state.mu.Unlock()
			case backend.EventDone:
				state.mu.Lock()
				if !sameActiveTurn(state.turn, turn) {
					state.mu.Unlock()
					continue
				}
				turn = state.turn
				if len(turn.pendingOutput) != 0 {
					payload := append([]byte(nil), turn.pendingOutput...)
					if err := sendDownlinkPCM(ctx, connection, state, turn, payload, true); err != nil {
						state.mu.Unlock()
						return err
					}
					turn.outputSequence++
					turn.pendingOutput = nil
				}
				if err := sendControl(ctx, connection, state, "response.done", turn, map[string]any{"response_id": event.ResponseID}); err != nil {
					state.mu.Unlock()
					return err
				}
				turn.active = false
				state.turn = turn
				state.mu.Unlock()
			case backend.EventError:
				state.mu.Lock()
				if !sameActiveTurn(state.turn, turn) {
					state.mu.Unlock()
					continue
				}
				if err := sendControl(ctx, connection, state, "error", turn, map[string]any{
					"code": "provider_error", "message": "AI provider request failed",
				}); err != nil {
					state.mu.Unlock()
					return err
				}
				state.turn.active = false
				state.turn.pendingOutput = nil
				state.mu.Unlock()
			}
		}
	}
}

func validateProviderAudio(sampleRateHz int, pcm []byte) error {
	// A provider delta is not a protocol PCM packet. Qwen may deliver several
	// wire frames in one delta, so the wire payload limit applies only after
	// the residual-aware rechunking in forwardEvents.
	if sampleRateHz != outputSampleRateHz || len(pcm) == 0 || len(pcm)%pcmBytesPerSample != 0 {
		return errors.New("provider returned invalid 24 kHz mono S16_LE audio")
	}
	return nil
}

func sendDownlinkPCM(ctx context.Context, connection *transport.Connection, state *connectionState, turn activeTurn, payload []byte, final bool) error {
	if len(payload) == 0 || len(payload) > outputFrameBytes || len(payload)%pcmBytesPerSample != 0 {
		return fmt.Errorf("downlink PCM payload has %d bytes; want one non-empty aligned frame of at most %d bytes", len(payload), outputFrameBytes)
	}
	flags := uint16(0)
	if turn.outputSequence == 0 {
		flags |= protocol.PCMFlagStart
	}
	if final {
		flags |= protocol.PCMFlagEnd
	}
	header := protocol.PCMHeader{
		Version: protocol.Version, Kind: protocol.AudioKindDownlink, Flags: flags,
		AudioFormat: protocol.AudioFormatPCM16LE, Channels: 1, SampleRateHz: outputSampleRateHz,
		PayloadLen: uint32(len(payload)), Sequence: turn.outputSequence,
		TimestampUS: state.timestampUS(), Epoch: turn.epoch, DeviceUUID: turn.deviceUUID,
		SessionID: turn.sessionID, TurnID: turn.turnID, StreamID: turn.downlinkStream,
	}
	return connection.SendPCM(ctx, header, payload)
}

func (state *connectionState) timestampUS() uint64 {
	elapsed := time.Since(state.monotonicStart)
	if elapsed <= 0 {
		return 0
	}
	return uint64(elapsed / time.Microsecond)
}

func validateActiveEnvelope(turn activeTurn, envelope protocol.ControlEnvelope) error {
	if !turn.active || envelope.TurnID != turn.turnID || envelope.StreamID != turn.uplinkStream || envelope.Epoch != turn.epoch {
		return errors.New("control message does not match the active turn")
	}
	return nil
}

func sameTurn(current activeTurn, expected activeTurn) bool {
	return current.sessionID == expected.sessionID && current.turnID == expected.turnID && current.epoch == expected.epoch
}

func sameActiveTurn(current activeTurn, expected activeTurn) bool {
	return current.active && sameTurn(current, expected)
}

func sendControlIfActive(ctx context.Context, connection *transport.Connection, state *connectionState, messageType string, turn activeTurn, payload any) error {
	state.mu.Lock()
	defer state.mu.Unlock()
	if !sameActiveTurn(state.turn, turn) {
		return nil
	}
	return sendControl(ctx, connection, state, messageType, turn, payload)
}

func redactedDeviceRef(deviceID string) string {
	digest := sha256.Sum256([]byte(deviceID))
	return fmt.Sprintf("%x", digest[:4])
}

func sendProtocolError(ctx context.Context, connection *transport.Connection, state *connectionState, err error) error {
	state.mu.Lock()
	turn := state.turn
	state.mu.Unlock()
	return sendControl(ctx, connection, state, "error", turn, map[string]any{"code": "protocol_error", "message": err.Error()})
}

func sendControl(ctx context.Context, connection *transport.Connection, state *connectionState, messageType string, turn activeTurn, payload any) error {
	encoded, err := json.Marshal(payload)
	if err != nil {
		return err
	}
	messageID := state.message.Add(1)
	return connection.SendControl(ctx, protocol.ControlEnvelope{
		Version: protocol.Version, Type: messageType, MessageID: fmt.Sprintf("server-%d", messageID), DeviceID: turn.deviceID,
		SessionID: turn.sessionID, TurnID: turn.turnID, StreamID: turn.downlinkStream, Epoch: turn.epoch, Payload: encoded,
	})
}

func randomNonzeroID() (uint32, error) {
	var data [4]byte
	for {
		if _, err := rand.Read(data[:]); err != nil {
			return 0, fmt.Errorf("generate session identifier: %w", err)
		}
		if value := binary.BigEndian.Uint32(data[:]); value != 0 {
			return value, nil
		}
	}
}
