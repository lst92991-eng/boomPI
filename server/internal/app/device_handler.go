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
	"net"
	"sync"
	"sync/atomic"
	"time"

	"github.com/gorilla/websocket"
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
	outputFrameDuration        = time.Duration(outputFrameDurationMS) * time.Millisecond
	minimumOutputFrameSpacing  = 15 * time.Millisecond
	downlinkMaxBufferedBytes   = outputSampleRateHz * pcmBytesPerSample * 1500 / 1000
	maxProviderAudioDeltaBytes = 64 * 1024
	downlinkReadResumeBytes    = downlinkMaxBufferedBytes - maxProviderAudioDeltaBytes
	helloAuthenticationTimeout = 5 * time.Second
)

var (
	errDeviceAuthentication     = errors.New("device authentication failed")
	errDeviceSessionIdleTimeout = errors.New("device session idle timeout")
	errInvalidTurnStart         = errors.New("invalid turn.start")
)

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
	outputStarted  bool
	committedAt    time.Time
	downlinkCtx    context.Context
	stopDownlink   context.CancelFunc
	active         bool
}

// pacedDownlink is deliberately local to the device handler. Provider audio
// may arrive much faster than a speaker can consume it, but the wire must not
// turn that burst into seconds of stale client-side audio. No complete frame
// should wait for a later provider delta merely to discover whether it is
// terminal. When done arrives after an exact frame boundary, one silent
// S16_LE sample carries the required non-empty END packet instead.
type pacedDownlink struct {
	epoch                uint32
	turnID               uint32
	responseID           string
	pending              bytes.Buffer
	providerDone         bool
	nextFrameAt          time.Time
	providerStartedAt    time.Time
	firstProviderAudioAt time.Time
	firstDownlinkQueueAt time.Time
	highWaterBytes       int
	wireFrames           uint32
}

func (p *pacedDownlink) reset() {
	p.epoch = 0
	p.turnID = 0
	p.responseID = ""
	p.pending.Reset()
	p.providerDone = false
	p.nextFrameAt = time.Time{}
	p.providerStartedAt = time.Time{}
	p.firstProviderAudioAt = time.Time{}
	p.firstDownlinkQueueAt = time.Time{}
	p.highWaterBytes = 0
	p.wireFrames = 0
}

func (p *pacedDownlink) begin(turn activeTurn, responseID string, now time.Time) {
	p.reset()
	p.epoch = turn.epoch
	p.turnID = turn.turnID
	p.responseID = responseID
	p.providerStartedAt = now
}

func (p *pacedDownlink) belongsTo(turn activeTurn) bool {
	return p.epoch != 0 && p.epoch == turn.epoch && p.turnID == turn.turnID
}

func (p *pacedDownlink) matches(turn activeTurn, responseID string) bool {
	return p.belongsTo(turn) && p.responseID == responseID
}

func (p *pacedDownlink) frameReady() bool {
	buffered := p.pending.Len()
	return buffered >= outputFrameBytes || (p.providerDone && buffered != 0)
}

type connectionState struct {
	mu             sync.Mutex
	turn           activeTurn
	sessionEpoch   uint32
	message        atomic.Uint64
	monotonicStart time.Time
}

func (h *deviceHandler) Handle(ctx context.Context, connection *transport.Connection) (result error) {
	deviceRef := "unidentified"
	sessionCtx := ctx
	defer func() {
		if errorCode, report := deviceSessionErrorCode(sessionCtx, result); report {
			h.logger.Warn("device session ended with error", "device_ref", deviceRef,
				"error_code", errorCode)
		}
	}()
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
	defer func() {
		if closeErr := actor.Close(); result == nil && closeErr != nil {
			result = closeErr
		}
	}()

	state := &connectionState{
		turn:           activeTurn{deviceID: hello.DeviceID, deviceUUID: deviceUUID, sessionID: sessionID, epoch: 1},
		sessionEpoch:   1,
		monotonicStart: time.Now(),
	}
	if err := sendControl(ctx, connection, state, "hello.ack", state.turn, map[string]any{
		"input_sample_rate_hz":  16_000,
		"output_sample_rate_hz": 24_000,
		"input_frame_ms":        20,
	}); err != nil {
		return err
	}
	deviceRef = redactedDeviceRef(hello.DeviceID)
	h.logger.Info("device connected", "device_ref", deviceRef)
	defer h.logger.Info("device disconnected", "device_ref", deviceRef)

	workerCtx, stopWorker := context.WithCancelCause(ctx)
	sessionCtx = workerCtx
	workerDone := make(chan error, 1)
	activity := make(chan struct{}, 1)
	idleDone := make(chan struct{})
	go func() {
		defer close(idleDone)
		watchDeviceSessionIdle(workerCtx, h.cfg.SessionIdleTimeout, activity, stopWorker)
	}()
	go func() {
		err := h.forwardEvents(workerCtx, connection, actor, state, activity)
		workerDone <- err
		stopWorker(err)
	}()
	defer func() {
		stopWorker(context.Canceled)
		<-workerDone
		<-idleDone
	}()

	for {
		message, receiveErr := connection.Receive(workerCtx)
		if receiveErr != nil {
			var congestion *transport.TurnCongestionError
			if errors.As(receiveErr, &congestion) {
				signalDeviceSessionActivity(activity)
				if err := h.handleTurnCongestion(workerCtx, connection, actor, state, congestion); err != nil {
					return err
				}
				continue
			}
			return receiveErr
		}
		signalDeviceSessionActivity(activity)
		if message.Control != nil {
			if staleRetiredTurnControl(state, *message.Control) {
				continue
			}
			if err := h.handleControl(workerCtx, connection, actor, state, *message.Control); err != nil {
				if errors.Is(err, errInvalidTurnStart) &&
					controlBelongsToDeviceSession(state, *message.Control) {
					if rejectErr := rejectTurnStart(workerCtx, connection, state, *message.Control); rejectErr != nil {
						return rejectErr
					}
					continue
				}
				if controlBelongsToActiveTurn(state, *message.Control) {
					if retireErr := h.retireActiveTurn(workerCtx, connection, actor, state, err); retireErr != nil {
						return retireErr
					}
					continue
				}
				_ = sendProtocolError(workerCtx, connection, state, err)
				return err
			}
			continue
		}
		if pcmBelongsToRetiredTurn(state, *message.PCMHeader) {
			continue
		}
		if err := h.handlePCM(workerCtx, actor, state, *message.PCMHeader, message.PCM); err != nil {
			if pcmBelongsToActiveTurn(state, *message.PCMHeader) {
				if retireErr := h.retireActiveTurn(workerCtx, connection, actor, state, err); retireErr != nil {
					return retireErr
				}
				continue
			}
			_ = sendProtocolError(workerCtx, connection, state, err)
			return err
		}
	}
}

func (h *deviceHandler) handleTurnCongestion(
	ctx context.Context,
	connection *transport.Connection,
	actor *session.Actor,
	state *connectionState,
	congestion *transport.TurnCongestionError,
) error {
	state.mu.Lock()
	turn := state.turn
	state.mu.Unlock()
	if congestion.SessionID != turn.sessionID {
		return errors.New("receive congestion does not belong to this device session")
	}
	if turn.active && congestion.TurnID == turn.turnID && congestion.Epoch == turn.epoch {
		return h.retireActiveTurn(ctx, connection, actor, state, congestion)
	}

	// The queue can fill before turn.start reaches the handler. Reject that
	// bounded turn explicitly and keep the authenticated provider session alive.
	rejected := turn
	rejected.turnID = congestion.TurnID
	rejected.uplinkStream = congestion.StreamID
	rejected.downlinkStream = congestion.StreamID
	rejected.epoch = congestion.Epoch
	rejected.active = false
	return sendControl(ctx, connection, state, "error", rejected, map[string]any{
		"code": "turn_error", "message": "The current voice turn was cancelled",
	})
}

// A malformed message inside the current turn must not tear down the
// authenticated WebSocket or the persistent provider session. Fence output,
// tell the client that this turn failed, then return the actor to idle.
func (h *deviceHandler) retireActiveTurn(
	ctx context.Context,
	connection *transport.Connection,
	actor *session.Actor,
	state *connectionState,
	cause error,
) error {
	state.mu.Lock()
	turn := state.turn
	if !turn.active {
		state.mu.Unlock()
		return cause
	}
	state.turn.active = false
	state.turn.pendingPCM = nil
	stopDownlink := state.turn.stopDownlink
	state.mu.Unlock()

	if stopDownlink != nil {
		stopDownlink()
	}
	if err := actor.Cancel(ctx, uint64(turn.epoch)); err != nil {
		return err
	}
	if err := sendControl(ctx, connection, state, "error", turn, map[string]any{
		"code": "turn_error", "message": "The current voice turn was cancelled",
	}); err != nil {
		return err
	}
	h.logger.Warn("voice turn retired after an invalid turn message",
		"device_ref", redactedDeviceRef(turn.deviceID), "turn_id", turn.turnID,
		"error_code", "invalid_turn_message")
	return nil
}

func controlBelongsToActiveTurn(state *connectionState, envelope protocol.ControlEnvelope) bool {
	state.mu.Lock()
	defer state.mu.Unlock()
	turn := state.turn
	return turn.active && envelope.DeviceID == turn.deviceID &&
		envelope.SessionID == turn.sessionID && envelope.TurnID == turn.turnID &&
		envelope.Epoch == turn.epoch &&
		(envelope.StreamID == turn.uplinkStream || envelope.StreamID == turn.downlinkStream)
}

func controlBelongsToDeviceSession(state *connectionState, envelope protocol.ControlEnvelope) bool {
	state.mu.Lock()
	defer state.mu.Unlock()
	return envelope.DeviceID == state.turn.deviceID && envelope.SessionID == state.turn.sessionID
}

func rejectTurnStart(
	ctx context.Context,
	connection *transport.Connection,
	state *connectionState,
	envelope protocol.ControlEnvelope,
) error {
	state.mu.Lock()
	turn := state.turn
	sessionEpoch := state.sessionEpoch
	state.mu.Unlock()
	if envelope.TurnID != 0 && envelope.StreamID != 0 && envelope.Epoch != 0 {
		turn.turnID = envelope.TurnID
		turn.uplinkStream = envelope.StreamID
		turn.downlinkStream = envelope.StreamID
		turn.epoch = envelope.Epoch
	} else {
		// A malformed start without a complete turn identity can only receive a
		// session-scoped error that the strict client can authenticate.
		turn.turnID = 0
		turn.uplinkStream = 0
		turn.downlinkStream = 0
		turn.epoch = sessionEpoch
	}
	turn.active = false
	return sendControl(ctx, connection, state, "error", turn, map[string]any{
		"code": "turn_error", "message": "The requested voice turn was rejected",
	})
}

func pcmBelongsToActiveTurn(state *connectionState, header protocol.PCMHeader) bool {
	state.mu.Lock()
	defer state.mu.Unlock()
	turn := state.turn
	return turn.active && header.DeviceUUID == turn.deviceUUID &&
		header.SessionID == turn.sessionID && header.TurnID == turn.turnID &&
		header.StreamID == turn.uplinkStream && header.Epoch == turn.epoch
}

// Once a turn is retired, frames already read from the socket may still be in
// the bounded receive queue. They are expected in-flight data, not a new
// session protocol violation. A later turn.start has a different identity and
// therefore remains visible to the state machine.
func pcmBelongsToRetiredTurn(state *connectionState, header protocol.PCMHeader) bool {
	state.mu.Lock()
	defer state.mu.Unlock()
	turn := state.turn
	return !turn.active && header.DeviceUUID == turn.deviceUUID &&
		header.SessionID == turn.sessionID && header.TurnID == turn.turnID &&
		header.StreamID == turn.uplinkStream && header.Epoch == turn.epoch
}

func staleRetiredTurnControl(state *connectionState, envelope protocol.ControlEnvelope) bool {
	if envelope.Type != "turn.commit" {
		return false
	}
	state.mu.Lock()
	defer state.mu.Unlock()
	turn := state.turn
	return !turn.active && envelope.DeviceID == turn.deviceID &&
		envelope.SessionID == turn.sessionID && envelope.TurnID == turn.turnID &&
		envelope.StreamID == turn.uplinkStream && envelope.Epoch == turn.epoch
}

func deviceSessionErrorCode(ctx context.Context, result error) (string, bool) {
	if result == nil {
		return "", false
	}
	errorToClassify := result
	if errors.Is(result, context.Canceled) && ctx != nil {
		if cause := context.Cause(ctx); cause != nil {
			errorToClassify = cause
		}
	}
	if errors.Is(errorToClassify, context.Canceled) ||
		errors.Is(errorToClassify, io.EOF) {
		return "", false
	}
	if errors.Is(errorToClassify, errDeviceSessionIdleTimeout) {
		return "session_idle_timeout", true
	}
	var closeError *websocket.CloseError
	if errors.As(errorToClassify, &closeError) {
		if closeError.Code == websocket.CloseNormalClosure ||
			closeError.Code == websocket.CloseGoingAway {
			return "", false
		}
		return "peer_disconnected", true
	}
	if errors.Is(errorToClassify, context.DeadlineExceeded) {
		return "timeout", true
	}
	var networkError net.Error
	if errors.As(errorToClassify, &networkError) {
		if networkError.Timeout() {
			return "network_timeout", true
		}
		return "network_error", true
	}
	return "session_error", true
}

func signalDeviceSessionActivity(activity chan<- struct{}) {
	// Ping/pong is consumed inside transport and never reaches this business
	// activity channel.
	select {
	case activity <- struct{}{}:
	default:
	}
}

func watchDeviceSessionIdle(ctx context.Context, timeout time.Duration, activity <-chan struct{}, stop context.CancelCauseFunc) {
	timer := time.NewTimer(timeout)
	defer timer.Stop()
	for {
		select {
		case <-ctx.Done():
			return
		case <-activity:
			resetTimer(timer, timeout)
		case <-timer.C:
			stop(errDeviceSessionIdleTimeout)
			return
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
			return fmt.Errorf("%w: a turn is already active", errInvalidTurnStart)
		}
		if envelope.TurnID == 0 || envelope.StreamID == 0 || envelope.Epoch == 0 {
			return fmt.Errorf("%w: identifiers must be nonzero", errInvalidTurnStart)
		}
		var fields map[string]json.RawMessage
		if err := json.Unmarshal(envelope.Payload, &fields); err != nil ||
			len(fields) != 1 || fields["sample_rate_hz"] == nil {
			return fmt.Errorf("%w: sample_rate_hz must be 16000", errInvalidTurnStart)
		}
		var sampleRateHz uint32
		if err := json.Unmarshal(fields["sample_rate_hz"], &sampleRateHz); err != nil ||
			sampleRateHz != 16_000 {
			return fmt.Errorf("%w: sample_rate_hz must be 16000", errInvalidTurnStart)
		}
		downlinkStream, err := randomNonzeroID()
		if err != nil {
			return err
		}
		if err := actor.StartTurn(ctx, uint64(envelope.Epoch)); err != nil {
			if errors.Is(err, session.ErrInvalidTransition) || errors.Is(err, session.ErrStaleEpoch) {
				return fmt.Errorf("%w: provider session rejected the turn epoch", errInvalidTurnStart)
			}
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
		turn.outputStarted = false
		turn.committedAt = time.Time{}
		if turn.stopDownlink != nil {
			turn.stopDownlink()
		}
		turn.downlinkCtx, turn.stopDownlink = context.WithCancel(ctx)
		turn.active = true
		state.mu.Lock()
		state.turn = turn
		state.mu.Unlock()
		return nil

	case "turn.commit":
		if err := validateActiveEnvelope(turn, envelope); err != nil {
			return err
		}
		if !emptyObjectPayload(envelope.Payload) {
			return errors.New("turn.commit payload must be empty")
		}
		if err := validateUplinkCommit(turn); err != nil {
			return err
		}
		if err := flushFinalInput(ctx, actor, &turn); err != nil {
			return err
		}
		turn.inputCommitted = true
		turn.committedAt = time.Now()
		state.mu.Lock()
		state.turn = turn
		state.mu.Unlock()
		return actor.Commit(ctx, uint64(turn.epoch))

	case "turn.cancel", "response.cancel":
		if !emptyObjectPayload(envelope.Payload) {
			return errors.New("cancel payload must be empty")
		}
		if !turn.active {
			if envelope.TurnID != turn.turnID || envelope.Epoch != turn.epoch ||
				(envelope.StreamID != turn.uplinkStream && envelope.StreamID != turn.downlinkStream) {
				return nil
			}
			// The provider may finish before the client drains the last audio.
			// A cancel for that retired epoch still means the user did not hear
			// the complete answer, so let the actor discard it from history.
			if err := actor.Cancel(ctx, uint64(turn.epoch)); err != nil {
				_ = sendControl(ctx, connection, state, "response.cancelled", turn,
					map[string]any{"reason": "client_request"})
				return err
			}
			return sendControl(ctx, connection, state, "response.cancelled", turn, map[string]any{"reason": "client_request"})
		}
		if envelope.TurnID != turn.turnID || envelope.Epoch != turn.epoch ||
			(envelope.StreamID != turn.uplinkStream && envelope.StreamID != turn.downlinkStream) {
			return errors.New("cancel identifiers do not match the active turn")
		}
		// Fence downlink immediately. Provider cancellation may wait for a remote
		// acknowledgement, but no event from this epoch should be queued meanwhile.
		state.mu.Lock()
		if sameTurn(state.turn, turn) {
			state.turn.active = false
			state.turn.pendingPCM = nil
			if state.turn.stopDownlink != nil {
				state.turn.stopDownlink()
			}
		}
		state.mu.Unlock()
		if err := actor.Cancel(ctx, uint64(turn.epoch)); err != nil {
			// The local downlink fence is already established. Acknowledge it even
			// when provider cleanup fails so the board does not wait for its full
			// cancel deadline before observing the subsequent session close.
			_ = sendControl(ctx, connection, state, "response.cancelled", turn,
				map[string]any{"reason": "client_request"})
			return err
		}
		turn.active = false
		turn.pendingPCM = nil
		return sendControl(ctx, connection, state, "response.cancelled", turn, map[string]any{"reason": "client_request"})
	default:
		return fmt.Errorf("unsupported control type %q", envelope.Type)
	}
}

func emptyObjectPayload(payload json.RawMessage) bool {
	var fields map[string]json.RawMessage
	return json.Unmarshal(payload, &fields) == nil && len(fields) == 0
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

func (h *deviceHandler) forwardEvents(ctx context.Context, connection *transport.Connection, actor *session.Actor, state *connectionState, activity chan<- struct{}) error {
	pacer := &pacedDownlink{}
	timer := time.NewTimer(time.Hour)
	stopTimer(timer)
	defer timer.Stop()

	for {
		state.mu.Lock()
		currentTurn := state.turn
		state.mu.Unlock()
		if pacer.epoch != 0 && (!currentTurn.active || !pacer.belongsTo(currentTurn)) {
			pacer.reset()
		}

		if pacer.providerDone && pacer.pending.Len() == 0 && pacer.wireFrames != 0 {
			// A complete frame was already released before the provider's done
			// event. Protocol v1 requires END on a non-empty aligned PCM packet,
			// so terminate with one inaudible zero sample instead of withholding
			// 20 ms of real audio as framing look-ahead.
			_, _ = pacer.pending.Write(make([]byte, pcmBytesPerSample))
		}
		if pacer.providerDone && pacer.pending.Len() == 0 {
			finished, err := h.finishPacedResponse(ctx, connection, state, pacer)
			if err != nil {
				return err
			}
			pacer.reset()
			if finished {
				continue
			}
		}

		var timerC <-chan time.Time
		if pacer.frameReady() {
			wait := time.Until(pacer.nextFrameAt)
			if pacer.nextFrameAt.IsZero() || wait <= 0 {
				sent, final, err := h.sendNextPacedFrame(ctx, connection, state, pacer)
				if err != nil {
					return err
				}
				if !sent {
					pacer.reset()
					continue
				}
				if final {
					if _, err := h.finishPacedResponse(ctx, connection, state, pacer); err != nil {
						return err
					}
					pacer.reset()
				}
				continue
			}
			timerC = resetTimer(timer, wait)
		} else {
			stopTimer(timer)
		}

		providerEvents := actor.Events()
		// Leave enough room for one maximum provider delta. Backpressure remains
		// inside the bounded server queues instead of becoming seconds of device
		// playback latency.
		if pacer.epoch != 0 && pacer.pending.Len() > downlinkReadResumeBytes {
			providerEvents = nil
		}

		select {
		case <-ctx.Done():
			return nil
		case <-timerC:
			continue
		case event, ok := <-providerEvents:
			if !ok {
				if ctx.Err() != nil {
					return nil
				}
				return errors.New("conversation session event stream closed unexpectedly")
			}
			state.mu.Lock()
			turn := state.turn
			state.mu.Unlock()
			if !turn.active || event.Epoch != uint64(turn.epoch) {
				continue
			}
			signalDeviceSessionActivity(activity)
			switch event.Type {
			case backend.EventStarted:
				if pacer.epoch != 0 {
					return errors.New("provider emitted duplicate response.start")
				}
				pacer.begin(turn, event.ResponseID, time.Now())
				if err := sendControlIfActive(ctx, connection, state, "response.start", turn, map[string]any{"response_id": event.ResponseID}); err != nil {
					return err
				}
			case backend.EventTextDelta:
				if !pacer.matches(turn, event.ResponseID) {
					return errors.New("provider text delta does not match the active response")
				}
				if err := sendControlIfActive(ctx, connection, state, "response.text_delta", turn, map[string]any{"response_id": event.ResponseID, "text": event.Text}); err != nil {
					return err
				}
			case backend.EventAudio:
				if err := validateProviderAudio(event.SampleRateHz, event.PCM); err != nil {
					return err
				}
				if !pacer.matches(turn, event.ResponseID) {
					return errors.New("provider audio arrived before response.start")
				}
				if pacer.pending.Len()+len(event.PCM) > downlinkMaxBufferedBytes {
					return fmt.Errorf("downlink pacing buffer exceeds %d ms", 1500)
				}
				state.mu.Lock()
				if !sameActiveTurn(state.turn, turn) {
					state.mu.Unlock()
					continue
				}
				turn = state.turn
				startAudio := !turn.outputStarted
				if !turn.outputStarted {
					turn.outputStarted = true
				}
				state.turn = turn
				state.mu.Unlock()
				if startAudio {
					if err := sendControlIfActive(ctx, connection, state, "response.audio_start", turn, map[string]any{
						"response_id": event.ResponseID, "sample_rate_hz": event.SampleRateHz,
					}); err != nil {
						return err
					}
				}
				if pacer.firstProviderAudioAt.IsZero() {
					pacer.firstProviderAudioAt = time.Now()
				}
				_, _ = pacer.pending.Write(event.PCM)
				if pacer.pending.Len() > pacer.highWaterBytes {
					pacer.highWaterBytes = pacer.pending.Len()
				}
			case backend.EventDone:
				if !pacer.matches(turn, event.ResponseID) {
					return errors.New("provider response.done arrived before response.start")
				}
				pacer.responseID = event.ResponseID
				pacer.providerDone = true
			case backend.EventError:
				state.mu.Lock()
				if !sameActiveTurn(state.turn, turn) {
					state.mu.Unlock()
					continue
				}
				turn = state.turn
				state.turn.active = false
				state.turn.pendingPCM = nil
				stopDownlink := state.turn.stopDownlink
				state.mu.Unlock()
				if stopDownlink != nil {
					stopDownlink()
				}
				if err := sendControl(ctx, connection, state, "error", turn, map[string]any{
					"code": "provider_error", "message": "AI provider request failed",
				}); err != nil {
					return err
				}
				pacer.reset()
			}
		}
	}
}

func (h *deviceHandler) sendNextPacedFrame(ctx context.Context, connection *transport.Connection, state *connectionState, pacer *pacedDownlink) (bool, bool, error) {
	return h.sendNextPacedFrameWith(ctx, state, pacer, func(
		sendCtx context.Context,
		turn activeTurn,
		payload []byte,
		final bool,
	) error {
		return sendDownlinkPCM(sendCtx, connection, state, turn, payload, final)
	})
}

func (h *deviceHandler) sendNextPacedFrameWith(
	ctx context.Context,
	state *connectionState,
	pacer *pacedDownlink,
	writePCM func(context.Context, activeTurn, []byte, bool) error,
) (bool, bool, error) {
	// This narrow seam makes the lock/backpressure boundary deterministic in a
	// host test. Production always supplies sendDownlinkPCM; it is not a second
	// transport interface and must not acquire more call sites.
	buffered := pacer.pending.Len()
	if buffered == 0 {
		return false, false, nil
	}
	payloadBytes := outputFrameBytes
	if buffered < payloadBytes {
		payloadBytes = buffered
	}
	final := pacer.providerDone && payloadBytes == buffered
	payload := pacer.pending.Bytes()[:payloadBytes]

	state.mu.Lock()
	if !state.turn.active || !pacer.belongsTo(state.turn) {
		state.mu.Unlock()
		return false, false, nil
	}
	turn := state.turn
	// Reserve this sequence while holding the state lock, then release it before
	// transport backpressure can block. Only forwardEvents reserves downlink
	// sequences, so the wire order remains deterministic.
	state.turn.outputSequence++
	state.mu.Unlock()

	if err := writePCM(turnContext(ctx, turn), turn, payload, final); err != nil {
		if !activeTurnMatches(state, turn) {
			return false, false, nil
		}
		return false, false, err
	}

	pacer.pending.Next(payloadBytes)
	pacer.wireFrames++
	now := time.Now()
	if pacer.firstDownlinkQueueAt.IsZero() {
		pacer.firstDownlinkQueueAt = now
	}
	pacer.nextFrameAt = nextPacedFrameDeadline(pacer.nextFrameAt, now)
	return true, final, nil
}

func nextPacedFrameDeadline(previousDeadline, sentAt time.Time) time.Time {
	// Preserve the 24 kHz media clock while preventing a late desktop timer
	// from being followed by a 5-10 ms catch-up burst.  Small overshoot may be
	// recovered gradually, but every next deadline keeps 15 ms of spacing.  A
	// full-frame stall rebases to a fresh 20 ms interval.
	if previousDeadline.IsZero() || sentAt.Sub(previousDeadline) >= outputFrameDuration {
		return sentAt.Add(outputFrameDuration)
	}
	cadenceDeadline := previousDeadline.Add(outputFrameDuration)
	minimumDeadline := sentAt.Add(minimumOutputFrameSpacing)
	if cadenceDeadline.Before(minimumDeadline) {
		return minimumDeadline
	}
	return cadenceDeadline
}

func (h *deviceHandler) finishPacedResponse(ctx context.Context, connection *transport.Connection, state *connectionState, pacer *pacedDownlink) (bool, error) {
	state.mu.Lock()
	if !state.turn.active || !pacer.belongsTo(state.turn) {
		state.mu.Unlock()
		return false, nil
	}
	turn := state.turn
	state.mu.Unlock()

	if err := sendControl(turnContext(ctx, turn), connection, state, "response.done", turn, map[string]any{"response_id": pacer.responseID}); err != nil {
		if !activeTurnMatches(state, turn) {
			return false, nil
		}
		return false, err
	}

	state.mu.Lock()
	if !sameActiveTurn(state.turn, turn) {
		state.mu.Unlock()
		return false, nil
	}
	turn = state.turn
	turn.active = false
	state.turn = turn
	stopDownlink := turn.stopDownlink
	state.mu.Unlock()
	if stopDownlink != nil {
		stopDownlink()
	}

	h.logger.Info("voice response timing",
		"device_ref", redactedDeviceRef(turn.deviceID),
		"turn_id", turn.turnID,
		"commit_to_provider_start_ms", elapsedMilliseconds(turn.committedAt, pacer.providerStartedAt),
		"commit_to_first_provider_audio_ms", elapsedMilliseconds(turn.committedAt, pacer.firstProviderAudioAt),
		"commit_to_first_downlink_enqueue_ms", elapsedMilliseconds(turn.committedAt, pacer.firstDownlinkQueueAt),
		"response_total_ms", elapsedMilliseconds(turn.committedAt, time.Now()),
		"local_downlink_buffer_high_water_ms", pacer.highWaterBytes*1000/(outputSampleRateHz*pcmBytesPerSample),
		"downlink_wire_frames", pacer.wireFrames,
	)
	return true, nil
}

func elapsedMilliseconds(start time.Time, end time.Time) int64 {
	if start.IsZero() || end.IsZero() || end.Before(start) {
		return -1
	}
	return end.Sub(start).Milliseconds()
}

func resetTimer(timer *time.Timer, wait time.Duration) <-chan time.Time {
	stopTimer(timer)
	timer.Reset(wait)
	return timer.C
}

func stopTimer(timer *time.Timer) {
	if !timer.Stop() {
		select {
		case <-timer.C:
		default:
		}
	}
}

func validateProviderAudio(sampleRateHz int, pcm []byte) error {
	// A provider delta is not a protocol PCM packet. Qwen may deliver several
	// wire frames in one delta, so the wire payload limit applies only after
	// the residual-aware rechunking in forwardEvents.
	if sampleRateHz != outputSampleRateHz || len(pcm) == 0 || len(pcm)%pcmBytesPerSample != 0 || len(pcm) > maxProviderAudioDeltaBytes {
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
	if !sameActiveTurn(state.turn, turn) {
		state.mu.Unlock()
		return nil
	}
	turn = state.turn
	state.mu.Unlock()
	if err := sendControl(turnContext(ctx, turn), connection, state, messageType, turn, payload); err != nil {
		if !activeTurnMatches(state, turn) {
			return nil
		}
		return err
	}
	return nil
}

func activeTurnMatches(state *connectionState, turn activeTurn) bool {
	state.mu.Lock()
	defer state.mu.Unlock()
	return sameActiveTurn(state.turn, turn)
}

func turnContext(fallback context.Context, turn activeTurn) context.Context {
	if turn.downlinkCtx != nil {
		return turn.downlinkCtx
	}
	return fallback
}

func redactedDeviceRef(deviceID string) string {
	digest := sha256.Sum256([]byte(deviceID))
	return fmt.Sprintf("%x", digest[:4])
}

func sendProtocolError(ctx context.Context, connection *transport.Connection, state *connectionState, _ error) error {
	state.mu.Lock()
	turn := state.turn
	turn.turnID = 0
	turn.uplinkStream = 0
	turn.downlinkStream = 0
	turn.epoch = state.sessionEpoch
	state.mu.Unlock()
	return sendControl(ctx, connection, state, "error", turn, map[string]any{
		"code": "protocol_error", "message": "Protocol message rejected",
	})
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
