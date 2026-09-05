package app

import (
	"bytes"
	"context"
	"crypto/sha256"
	"crypto/subtle"
	"errors"
	"fmt"
	"log/slog"
	"time"
	"unicode/utf8"

	"github.com/lst92991-eng/boomPI/server/internal/backend"
	"github.com/lst92991-eng/boomPI/server/internal/config"
	"github.com/lst92991-eng/boomPI/server/internal/protocol"
	"github.com/lst92991-eng/boomPI/server/internal/session"
	"github.com/lst92991-eng/boomPI/server/internal/transport"
)

const (
	helloAuthenticationTimeout = 5 * time.Second
	maxTextDeltaBytes          = protocol.MaxTextBytes
	outputFrameDuration        = 20 * time.Millisecond
	minimumOutputFrameSpacing  = 15 * time.Millisecond
	downlinkMaxBufferedBytes   = 8 * protocol.DownlinkFrameBytes
)

type deviceHandler struct {
	cfg      config.Config
	logger   *slog.Logger
	provider backend.ConversationBackend
}

// Handle reads the teaching conversation in wire order. START begins input,
// END commits it, and a newer START/STOP retires all earlier generations.
// Provider operations run only in session.Actor, so cancellation cannot block
// this reader or its bounded input queues.
func (h *deviceHandler) Handle(ctx context.Context, c *transport.Connection) error {
	helloCtx, cancel := context.WithTimeout(ctx, helloAuthenticationTimeout)
	first, err := c.Receive(helloCtx)
	cancel()
	if err != nil {
		return err
	}
	if first.Control == nil || first.Control.Type != "hello" {
		return errors.New("first message must be hello")
	}
	hello := first.Control
	if subtle.ConstantTimeCompare([]byte(hello.Token), []byte(h.cfg.DeviceToken.Value())) != 1 {
		return errors.New("device authentication failed")
	}
	actor, err := session.Open(ctx, h.provider, backend.SessionConfig{
		DeviceID: hello.DeviceID, SystemPrompt: h.cfg.SystemPrompt, Persona: h.cfg.Persona,
	})
	if err != nil {
		return errors.New("provider session setup failed")
	}
	defer actor.Close()
	if err = c.SendControl(ctx, protocol.Control{Type: "ready"}); err != nil {
		return err
	}
	deviceRef := redactedDeviceRef(hello.DeviceID)
	h.logger.Info("device connected", "device_ref", deviceRef)
	defer h.logger.Info("device disconnected", "device_ref", deviceRef)

	runCtx, stop := context.WithCancel(ctx)
	forwardDone := make(chan error, 1)
	go func() { forwardDone <- h.forwardEvents(runCtx, c, actor); stop() }()
	defer func() { stop(); <-forwardDone }()

	var generation, sequence uint32
	inputEnded := true
	for {
		idleCtx, cancel := context.WithTimeout(runCtx, h.cfg.SessionIdleTimeout)
		message, err := c.Receive(idleCtx)
		cancel()
		if err != nil {
			return err
		}
		if message.Control != nil {
			control := message.Control
			if control.Type != "stop" {
				return errors.New("hello may only occur once")
			}
			if control.Generation <= generation {
				continue
			}
			generation, sequence, inputEnded = control.Generation, 0, true
			if err = actor.Stop(generation, *control.Retract); err != nil {
				return err
			}
			continue
		}
		header := *message.PCMHeader
		if header.Generation < generation {
			continue
		}
		if header.Flags&protocol.PCMFlagStart != 0 {
			if header.Generation <= generation {
				return errors.New("generation reuse")
			}
			generation, sequence, inputEnded = header.Generation, 0, false
		}
		if header.Generation != generation || inputEnded || header.Sequence != sequence {
			return errors.New("uplink sequence gap or audio after END")
		}
		if sequence >= 3000 {
			return errors.New("input exceeds 60 seconds")
		}
		sequence++
		inputEnded = header.Flags&protocol.PCMFlagEnd != 0
		if err = actor.Submit(header, message.PCM); err != nil {
			return err
		}
	}
}

// pacedDownlink retains one 20 ms frame as framing lookahead. This lets the
// actual final PCM carry END, including a one-frame answer, without inventing
// silence or exposing response/audio-start messages to the client.
type pacedDownlink struct {
	generation uint32
	sequence   uint32
	pending    bytes.Buffer
	done       bool
	nextAt     time.Time
}

func (h *deviceHandler) forwardEvents(ctx context.Context, c *transport.Connection, actor *session.Actor) error {
	var output pacedDownlink
	timer := time.NewTimer(time.Hour)
	defer timer.Stop()
	for {
		if output.generation != 0 && output.generation != c.Generation() {
			output = pacedDownlink{}
		}
		if output.done && output.pending.Len() == 0 {
			if err := c.SendControl(ctx, protocol.Control{Type: "done", Generation: output.generation}); err != nil {
				return err
			}
			output = pacedDownlink{}
		}
		ready := output.pending.Len() > protocol.DownlinkFrameBytes || (output.done && output.pending.Len() > 0)
		var timerC <-chan time.Time
		if ready {
			wait := time.Until(output.nextAt)
			if output.nextAt.IsZero() || wait <= 0 {
				if err := sendPacedFrame(ctx, c, &output); err != nil {
					return err
				}
				continue
			}
			stopTimer(timer)
			timer.Reset(wait)
			timerC = timer.C
		} else {
			// A periodic wake also observes a newer socket fence while provider
			// output is full or its cancellation is still completing.
			stopTimer(timer)
			timer.Reset(outputFrameDuration)
			timerC = timer.C
		}
		events := actor.Events()
		if output.pending.Len() >= downlinkMaxBufferedBytes {
			events = nil
		}
		select {
		case <-ctx.Done():
			return nil
		case <-timerC:
		case event, ok := <-events:
			if !ok {
				return errors.New("provider session stopped")
			}
			if event.Generation != c.Generation() {
				continue
			}
			switch event.Type {
			case backend.EventStarted:
				if output.generation != 0 {
					return errors.New("duplicate provider start")
				}
				output = pacedDownlink{generation: event.Generation}
			case backend.EventTextDelta:
				chunks, err := splitTextDelta(event.Text)
				if err != nil {
					return err
				}
				for _, text := range chunks {
					if err = c.SendControl(ctx, protocol.Control{Type: "text", Generation: event.Generation, Text: text}); err != nil {
						return err
					}
				}
			case backend.EventAudio:
				if output.generation != event.Generation {
					return errors.New("audio without provider start")
				}
				_, _ = output.pending.Write(event.PCM)
			case backend.EventDone:
				if output.generation != event.Generation {
					return errors.New("done without provider start")
				}
				output.done = true
			case backend.EventError:
				output = pacedDownlink{}
				if err := c.SendControl(ctx, protocol.Control{Type: "error", Generation: event.Generation, Code: "provider_error"}); err != nil {
					return err
				}
			default:
				return errors.New("invalid provider event")
			}
		}
	}
}

func sendPacedFrame(ctx context.Context, c *transport.Connection, p *pacedDownlink) error {
	count := min(p.pending.Len(), protocol.DownlinkFrameBytes)
	flags := uint16(0)
	if p.sequence == 0 {
		flags |= protocol.PCMFlagStart
	}
	if p.done && count == p.pending.Len() {
		flags |= protocol.PCMFlagEnd
	}
	header := protocol.PCMHeader{Generation: p.generation, Sequence: p.sequence, Flags: flags}
	if err := c.SendPCM(ctx, header, p.pending.Bytes()[:count]); err != nil {
		return err
	}
	p.pending.Next(count)
	p.sequence++
	p.nextAt = nextPacedFrameDeadline(p.nextAt, time.Now())
	return nil
}

func nextPacedFrameDeadline(previous, sentAt time.Time) time.Time {
	if previous.IsZero() || sentAt.Sub(previous) >= outputFrameDuration {
		return sentAt.Add(outputFrameDuration)
	}
	deadline := previous.Add(outputFrameDuration)
	if minimum := sentAt.Add(minimumOutputFrameSpacing); deadline.Before(minimum) {
		return minimum
	}
	return deadline
}

func stopTimer(timer *time.Timer) {
	if !timer.Stop() {
		select {
		case <-timer.C:
		default:
		}
	}
}

func splitTextDelta(text string) ([]string, error) {
	if text == "" || !utf8.ValidString(text) {
		return nil, errors.New("invalid provider UTF-8 text")
	}
	var chunks []string
	for len(text) > maxTextDeltaBytes {
		end := maxTextDeltaBytes
		for !utf8.RuneStart(text[end]) {
			end--
		}
		chunks = append(chunks, text[:end])
		text = text[end:]
	}
	return append(chunks, text), nil
}

func redactedDeviceRef(deviceID string) string {
	hash := sha256.Sum256([]byte(deviceID))
	return fmt.Sprintf("%x", hash[:4])
}
