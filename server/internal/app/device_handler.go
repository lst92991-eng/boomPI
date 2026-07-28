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
	outputSequence uint32
	pendingPCM     []byte
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
		turn.outputSequence = 0
		turn.pendingPCM = nil
		turn.active = true
		state.mu.Lock()
		state.turn = turn
		state.mu.Unlock()
		return nil

	case "turn.commit":
		if err := validateActiveEnvelope(turn, envelope); err != nil {
			return err
		}
		if err := flushFinalInput(ctx, actor, &turn); err != nil {
			return err
		}
		state.mu.Lock()
		state.turn = turn
		state.mu.Unlock()
		return actor.Commit(ctx, uint64(turn.epoch))

	case "turn.cancel", "response.cancel":
		if !turn.active {
			return nil
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
		}
		state.mu.Unlock()
		if err := actor.Cancel(ctx, uint64(turn.epoch)); err != nil {
			return err
		}
		turn.active = false
		turn.pendingPCM = nil
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
				state.mu.Lock()
				if !sameActiveTurn(state.turn, turn) {
					state.mu.Unlock()
					continue
				}
				turn = state.turn
				if turn.outputSequence == 0 {
					if err := sendControl(ctx, connection, state, "response.audio_start", turn, map[string]any{"response_id": event.ResponseID, "sample_rate_hz": event.SampleRateHz}); err != nil {
						state.mu.Unlock()
						return err
					}
				}
				header := protocol.PCMHeader{
					Version: protocol.Version, Kind: protocol.AudioKindDownlink, AudioFormat: protocol.AudioFormatPCM16LE,
					Channels: 1, SampleRateHz: uint32(event.SampleRateHz), PayloadLen: uint32(len(event.PCM)),
					Sequence: turn.outputSequence, TimestampUS: state.timestampUS(), Epoch: turn.epoch,
					DeviceUUID: turn.deviceUUID, SessionID: turn.sessionID, TurnID: turn.turnID, StreamID: turn.downlinkStream,
				}
				if err := connection.SendPCM(ctx, header, event.PCM); err != nil {
					state.mu.Unlock()
					return err
				}
				state.turn.outputSequence++
				state.mu.Unlock()
			case backend.EventDone:
				state.mu.Lock()
				if !sameActiveTurn(state.turn, turn) {
					state.mu.Unlock()
					continue
				}
				if err := sendControl(ctx, connection, state, "response.done", turn, map[string]any{"response_id": event.ResponseID}); err != nil {
					state.mu.Unlock()
					return err
				}
				state.turn.active = false
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
				state.mu.Unlock()
			}
		}
	}
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
