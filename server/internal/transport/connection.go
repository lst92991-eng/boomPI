package transport

import (
	"context"
	"errors"
	"fmt"
	"io"
	"sync"
	"sync/atomic"
	"time"

	"github.com/gorilla/websocket"
	"github.com/lst92991-eng/boomPI/server/internal/protocol"
)

var errConnectionClosed = errors.New("transport connection is closed")

// Message contains one validated v2 JSON object or one fixed 20 ms uplink frame.
type Message struct {
	Control   *protocol.Control
	PCMHeader *protocol.PCMHeader
	PCM       []byte
}

func (m Message) generation() uint32 {
	if m.Control != nil {
		return m.Control.Generation
	}
	return m.PCMHeader.Generation
}

func (m Message) boundary() bool {
	return m.Control != nil || m.PCMHeader.Flags&protocol.PCMFlagStart != 0
}

type outboundMessage struct {
	messageType int
	data        []byte
	generation  uint32
}

// The handler is the sole socket reader; this package owns one writer. The
// provider actor owns the only input queue, avoiding a second queue that could
// reject a valid 500 ms pre-roll burst before the actor had a chance to run.
type Connection struct {
	webSocket    *websocket.Conn
	config       Config
	ctx          context.Context
	cancel       context.CancelCauseFunc
	done         chan struct{}
	sendQueue    chan outboundMessage
	controlQueue chan outboundMessage
	fence        atomic.Uint32
	closeOnce    sync.Once
}

func newConnection(parent context.Context, socket *websocket.Conn, config Config) (*Connection, error) {
	ctx, cancel := context.WithCancelCause(parent)
	c := &Connection{
		webSocket: socket, config: config, ctx: ctx, cancel: cancel,
		done:         make(chan struct{}),
		sendQueue:    make(chan outboundMessage, sendQueueCapacity),
		controlQueue: make(chan outboundMessage, 2),
	}
	socket.SetReadLimit(protocol.MaxControlMessageBytes)
	if err := socket.SetReadDeadline(time.Now().Add(config.PongTimeout)); err != nil {
		cancel(err)
		return nil, err
	}
	socket.SetPongHandler(func(string) error { return socket.SetReadDeadline(time.Now().Add(config.PongTimeout)) })
	socket.SetPingHandler(func(data string) error {
		select {
		case c.controlQueue <- outboundMessage{messageType: websocket.PongMessage, data: []byte(data)}:
			return nil
		default:
			return errors.New("heartbeat queue full")
		}
	})
	socket.SetCloseHandler(func(code int, text string) error { return &websocket.CloseError{Code: code, Text: text} })
	go c.writePump()
	return c, nil
}

func (c *Connection) context() context.Context { return c.ctx }
func (c *Connection) Generation() uint32       { return c.fence.Load() }

func (c *Connection) Receive(ctx context.Context) (Message, error) {
	if err := context.Cause(c.ctx); err != nil {
		return Message{}, err
	}
	if err := ctx.Err(); err != nil {
		return Message{}, err
	}
	// Cancellation closes the writer/socket and unblocks NextReader. No helper
	// goroutine or provider call is added to the normal per-frame path.
	stopWatch := context.AfterFunc(ctx, func() { c.cancel(context.Cause(ctx)) })
	defer stopWatch()
	message, err := c.readMessage()
	if err != nil {
		c.cancel(err)
		return Message{}, err
	}
	generation := message.generation()
	if message.boundary() && generation > c.fence.Load() {
		c.fence.Store(generation)
	}
	return message, nil
}

func (c *Connection) SendControl(ctx context.Context, control protocol.Control) error {
	data, err := protocol.EncodeControl(control)
	if err != nil {
		return err
	}
	return c.enqueue(ctx, outboundMessage{messageType: websocket.TextMessage, data: data, generation: control.Generation})
}

func (c *Connection) SendPCM(ctx context.Context, header protocol.PCMHeader, pcm []byte) error {
	data, err := protocol.EncodePCM(header, pcm, false)
	if err != nil {
		return err
	}
	return c.enqueue(ctx, outboundMessage{messageType: websocket.BinaryMessage, data: data, generation: header.Generation})
}

func (c *Connection) enqueue(ctx context.Context, message outboundMessage) error {
	// A blocked peer cannot indefinitely hold the forwarding worker.
	timer := time.NewTimer(800 * time.Millisecond)
	defer timer.Stop()
	select {
	case c.sendQueue <- message:
		return nil
	case <-ctx.Done():
		return ctx.Err()
	case <-c.ctx.Done():
		return context.Cause(c.ctx)
	case <-timer.C:
		return errors.New("downlink queue timeout")
	}
}

func (c *Connection) Close() error {
	c.closeOnce.Do(func() { c.cancel(errConnectionClosed) })
	<-c.done
	return nil
}

func (c *Connection) readMessage() (Message, error) {
	kind, reader, err := c.webSocket.NextReader()
	if err != nil {
		return Message{}, err
	}
	limit := protocol.MaxControlMessageBytes
	if kind == websocket.BinaryMessage {
		limit = protocol.PCMHeaderSize + protocol.UplinkFrameBytes
	}
	data, err := readBounded(reader, limit)
	if err != nil {
		return Message{}, err
	}
	switch kind {
	case websocket.TextMessage:
		control, err := protocol.DecodeControl(data)
		if err != nil {
			return Message{}, err
		}
		if control.Type != "hello" && control.Type != "stop" {
			return Message{}, errors.New("unexpected client control")
		}
		return Message{Control: &control}, nil
	case websocket.BinaryMessage:
		header, pcm, err := protocol.ParsePCMFrame(data, true)
		return Message{PCMHeader: &header, PCM: pcm}, err
	default:
		return Message{}, errors.New("unexpected WebSocket message")
	}
}

func readBounded(reader io.Reader, maxBytes int) ([]byte, error) {
	data, err := io.ReadAll(io.LimitReader(reader, int64(maxBytes)+1))
	if err == nil && len(data) > maxBytes {
		err = fmt.Errorf("message exceeds %d bytes", maxBytes)
	}
	return data, err
}

func (c *Connection) writePump() {
	defer close(c.done)
	defer c.webSocket.Close()
	ticker := time.NewTicker(c.config.PingInterval)
	defer ticker.Stop()
	for {
		// Heartbeats retain priority even with continuously queued audio.
		select {
		case <-ticker.C:
			if err := c.write(outboundMessage{messageType: websocket.PingMessage}); err != nil {
				c.cancel(err)
				return
			}
		default:
		}
		select {
		case <-c.ctx.Done():
			return
		case message := <-c.controlQueue:
			if err := c.write(message); err != nil {
				c.cancel(err)
				return
			}
		case message := <-c.sendQueue:
			if err := c.write(message); err != nil {
				c.cancel(err)
				return
			}
		case <-ticker.C:
			if err := c.write(outboundMessage{messageType: websocket.PingMessage}); err != nil {
				c.cancel(err)
				return
			}
		}
	}
}

func (c *Connection) write(message outboundMessage) error {
	if message.generation != 0 && message.generation < c.fence.Load() {
		return nil
	}
	if err := c.webSocket.SetWriteDeadline(time.Now().Add(writeTimeout)); err != nil {
		return err
	}
	return c.webSocket.WriteMessage(message.messageType, message.data)
}
