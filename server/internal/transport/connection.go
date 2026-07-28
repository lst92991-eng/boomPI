package transport

import (
	"context"
	"errors"
	"fmt"
	"io"
	"sync"
	"time"

	"github.com/gorilla/websocket"

	"github.com/lst92991-eng/boomPI/server/internal/protocol"
)

const maxWireMessageBytes = protocol.PCMHeaderSize + protocol.MaxPCMPayloadBytes

var (
	errConnectionClosed = errors.New("transport connection is closed")
	errSendQueueFull    = errors.New("transport send queue is full")
)

// Message contains exactly one decoded control envelope or one parsed PCM frame.
type Message struct {
	Control   *protocol.ControlEnvelope
	PCMHeader *protocol.PCMHeader
	PCM       []byte
}

type outboundMessage struct {
	messageType int
	data        []byte
}

// Connection is a bounded, protocol-aware view of one device connection.
// One internal goroutine owns all WebSocket writes and another owns all reads.
type Connection struct {
	webSocket *websocket.Conn
	config    Config

	ctx    context.Context
	cancel context.CancelCauseFunc
	done   chan struct{}

	receiveQueue chan Message
	sendQueue    chan outboundMessage
	controlQueue chan outboundMessage

	closeOnce sync.Once
	waitGroup sync.WaitGroup
}

func newConnection(parent context.Context, webSocket *websocket.Conn, config Config) (*Connection, error) {
	ctx, cancel := context.WithCancelCause(parent)
	connection := &Connection{
		webSocket:    webSocket,
		config:       config,
		ctx:          ctx,
		cancel:       cancel,
		done:         make(chan struct{}),
		receiveQueue: make(chan Message, receiveQueueCapacity),
		sendQueue:    make(chan outboundMessage, sendQueueCapacity),
		controlQueue: make(chan outboundMessage, 2),
	}
	if err := connection.configureReadSide(); err != nil {
		cancel(err)
		return nil, err
	}
	connection.waitGroup.Add(2)
	go connection.readPump()
	go connection.writePump()
	go func() {
		connection.waitGroup.Wait()
		close(connection.done)
	}()
	return connection, nil
}

func (connection *Connection) context() context.Context {
	return connection.ctx
}

// Receive waits for the next validated protocol message or cancellation.
func (connection *Connection) Receive(ctx context.Context) (Message, error) {
	if ctx == nil {
		return Message{}, errors.New("receive context is required")
	}
	if err := context.Cause(connection.ctx); err != nil {
		return Message{}, err
	}
	select {
	case <-ctx.Done():
		return Message{}, context.Cause(ctx)
	case <-connection.ctx.Done():
		return Message{}, context.Cause(connection.ctx)
	case message := <-connection.receiveQueue:
		return message, nil
	}
}

// SendControl validates and queues one JSON control message without blocking.
func (connection *Connection) SendControl(ctx context.Context, envelope protocol.ControlEnvelope) error {
	encoded, err := protocol.EncodeControl(envelope)
	if err != nil {
		return fmt.Errorf("encode outbound control message: %w", err)
	}
	return connection.enqueue(ctx, outboundMessage{messageType: websocket.TextMessage, data: encoded})
}

// SendPCM validates and queues one complete binary PCM frame without blocking.
func (connection *Connection) SendPCM(ctx context.Context, header protocol.PCMHeader, payload []byte) error {
	if uint32(len(payload)) != header.PayloadLen {
		return errors.New("outbound PCM payload length does not match its header")
	}
	encodedHeader, err := header.MarshalBinary()
	if err != nil {
		return fmt.Errorf("encode outbound PCM header: %w", err)
	}
	frame := make([]byte, len(encodedHeader)+len(payload))
	copy(frame, encodedHeader)
	copy(frame[len(encodedHeader):], payload)
	return connection.enqueue(ctx, outboundMessage{messageType: websocket.BinaryMessage, data: frame})
}

// Close cancels I/O, sends a bounded close handshake, and waits for both pumps.
func (connection *Connection) Close() error {
	connection.closeOnce.Do(func() {
		connection.cancel(errConnectionClosed)
	})
	<-connection.done
	return nil
}

func (connection *Connection) enqueue(ctx context.Context, message outboundMessage) error {
	if ctx == nil {
		return errors.New("send context is required")
	}
	select {
	case <-ctx.Done():
		return context.Cause(ctx)
	case <-connection.ctx.Done():
		return context.Cause(connection.ctx)
	default:
	}
	select {
	case connection.sendQueue <- message:
		return nil
	default:
		return errSendQueueFull
	}
}

func (connection *Connection) configureReadSide() error {
	connection.webSocket.SetReadLimit(maxWireMessageBytes)
	if err := connection.webSocket.SetReadDeadline(time.Now().Add(connection.config.PongTimeout)); err != nil {
		return fmt.Errorf("set initial WebSocket read deadline: %w", err)
	}
	connection.webSocket.SetPongHandler(func(string) error {
		return connection.webSocket.SetReadDeadline(time.Now().Add(connection.config.PongTimeout))
	})
	connection.webSocket.SetPingHandler(func(data string) error {
		message := outboundMessage{messageType: websocket.PongMessage, data: []byte(data)}
		select {
		case connection.controlQueue <- message:
			return nil
		default:
			return errors.New("transport control queue is full")
		}
	})
	connection.webSocket.SetCloseHandler(func(code int, text string) error {
		return &websocket.CloseError{Code: code, Text: text}
	})
	return nil
}

func (connection *Connection) readPump() {
	defer connection.waitGroup.Done()
	for {
		message, err := connection.readMessage()
		if err != nil {
			connection.cancel(err)
			return
		}
		select {
		case connection.receiveQueue <- message:
		case <-connection.ctx.Done():
			return
		default:
			connection.cancel(errors.New("transport receive queue is full"))
			return
		}
	}
}

func (connection *Connection) readMessage() (Message, error) {
	messageType, reader, err := connection.webSocket.NextReader()
	if err != nil {
		return Message{}, fmt.Errorf("read WebSocket frame: %w", err)
	}
	switch messageType {
	case websocket.TextMessage:
		data, err := readBounded(reader, protocol.MaxControlMessageBytes)
		if err != nil {
			return Message{}, fmt.Errorf("read control frame: %w", err)
		}
		envelope, err := protocol.DecodeControl(data)
		if err != nil {
			return Message{}, fmt.Errorf("decode control frame: %w", err)
		}
		return Message{Control: &envelope}, nil
	case websocket.BinaryMessage:
		data, err := readBounded(reader, maxWireMessageBytes)
		if err != nil {
			return Message{}, fmt.Errorf("read PCM frame: %w", err)
		}
		header, payload, err := protocol.ParsePCMFrame(data)
		if err != nil {
			return Message{}, fmt.Errorf("decode PCM frame: %w", err)
		}
		return Message{PCMHeader: &header, PCM: payload}, nil
	default:
		return Message{}, fmt.Errorf("unsupported WebSocket message type %d", messageType)
	}
}

func readBounded(reader io.Reader, maxBytes int) ([]byte, error) {
	data, err := io.ReadAll(io.LimitReader(reader, int64(maxBytes)+1))
	if err != nil {
		return nil, err
	}
	if len(data) > maxBytes {
		return nil, fmt.Errorf("message exceeds %d bytes", maxBytes)
	}
	return data, nil
}

func (connection *Connection) writePump() {
	defer connection.waitGroup.Done()
	defer connection.webSocket.Close()
	ticker := time.NewTicker(connection.config.PingInterval)
	defer ticker.Stop()

	for {
		select {
		case message := <-connection.controlQueue:
			if err := connection.write(message); err != nil {
				connection.cancel(err)
				return
			}
		default:
		}

		select {
		case <-connection.ctx.Done():
			_ = connection.webSocket.SetWriteDeadline(time.Now().Add(writeTimeout))
			_ = connection.webSocket.WriteMessage(websocket.CloseMessage, websocket.FormatCloseMessage(websocket.CloseNormalClosure, "closing"))
			return
		case message := <-connection.controlQueue:
			if err := connection.write(message); err != nil {
				connection.cancel(err)
				return
			}
		case message := <-connection.sendQueue:
			if err := connection.write(message); err != nil {
				connection.cancel(err)
				return
			}
		case <-ticker.C:
			if err := connection.write(outboundMessage{messageType: websocket.PingMessage}); err != nil {
				connection.cancel(err)
				return
			}
		}
	}
}

func (connection *Connection) write(message outboundMessage) error {
	if err := connection.webSocket.SetWriteDeadline(time.Now().Add(writeTimeout)); err != nil {
		return fmt.Errorf("set WebSocket write deadline: %w", err)
	}
	if err := connection.webSocket.WriteMessage(message.messageType, message.data); err != nil {
		return fmt.Errorf("write WebSocket frame: %w", err)
	}
	return nil
}
