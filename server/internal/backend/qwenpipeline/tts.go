package qwenpipeline

import (
	"context"
	"crypto/rand"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"net/http"
	"strings"
	"sync"
	"time"
	"unicode"

	"github.com/gorilla/websocket"
)

const (
	maxTTSFragmentUnits         = 1600
	maxTTSPCMDeltaBytes         = 64 * 1024
	maxTTSWebSocketMessageBytes = 128 * 1024
)

type ttsClient struct {
	config Config
	dialer websocket.Dialer
}

func newTTSClient(config Config) *ttsClient {
	dialer := *websocket.DefaultDialer
	dialer.HandshakeTimeout = config.Timeout
	return &ttsClient{config: config, dialer: dialer}
}

func (c *ttsClient) synthesizeStream(ctx context.Context, fragments <-chan string, emit func([]byte) error) error {
	header := make(http.Header)
	header.Set("Authorization", "Bearer "+c.config.APIKey)
	if c.config.WorkspaceID != "" {
		header.Set("X-DashScope-WorkSpace", c.config.WorkspaceID)
	}
	connection, response, err := c.dialer.DialContext(ctx, c.config.ttsURL(), header)
	if response != nil && response.Body != nil {
		response.Body.Close()
	}
	if err != nil {
		return fmt.Errorf("connect CosyVoice TTS: %w", err)
	}
	defer connection.Close()
	return c.synthesizeConnectedStream(ctx, connection, fragments, emit)
}

// 一轮合成独占一个连接；取消发送官方 directive 并有界关闭连接，不复用旧任务。
// task-finished 只表示云端音频全部交付，扬声器尾播由客户端自行确认。
func (c *ttsClient) synthesizeConnectedStream(ctx context.Context, connection *websocket.Conn, fragments <-chan string, emit func([]byte) error) error {
	streamCtx, cancel := context.WithCancel(ctx)
	defer cancel()
	connection.SetReadLimit(maxTTSWebSocketMessageBytes)
	taskID := eventID()
	// 取消与正常文本共用写锁。150 ms 后强制关闭，即使云端不读也不会拖住新轮。
	var writeMu sync.Mutex
	write := func(action string, payload any) error {
		writeMu.Lock()
		defer writeMu.Unlock()
		if err := streamCtx.Err(); err != nil {
			return err
		}
		return c.writeTask(connection, taskID, action, payload)
	}
	watchStop, watchDone := make(chan struct{}), make(chan struct{})
	go func() {
		defer close(watchDone)
		select {
		case <-ctx.Done():
			forceClose := time.AfterFunc(150*time.Millisecond, func() { _ = connection.Close() })
			writeMu.Lock()
			_ = connection.SetWriteDeadline(time.Now().Add(100 * time.Millisecond))
			_ = connection.WriteJSON(taskMessage(taskID, "finish-task", map[string]any{
				"input": map[string]any{"directive": "cancel"},
			}))
			writeMu.Unlock()
			_ = connection.Close()
			forceClose.Stop()
		case <-watchStop:
		}
	}()
	defer func() {
		if ctx.Err() != nil {
			<-watchDone
		}
		close(watchStop)
		_ = connection.Close()
		<-watchDone
	}()
	if err := write("run-task", map[string]any{
		"task_group": "audio", "task": "tts", "function": "SpeechSynthesizer",
		"model": c.config.TTSModel, "input": map[string]any{},
		"parameters": map[string]any{
			"text_type": "PlainText", "voice": c.config.TTSVoice,
			"format": "pcm", "sample_rate": ttsSampleRateHz,
		},
	}); err != nil {
		return err
	}
	kind, pcm, err := c.readTask(streamCtx, connection, taskID)
	if err != nil {
		return err
	}
	if kind != "task-started" {
		return errors.New("CosyVoice TTS did not start the task")
	}
	writerDone := make(chan struct{})
	var writerErr error
	go func() {
		err := c.writeText(streamCtx, write, fragments)
		writerErr = err
		defer close(writerDone)
		if err != nil && ctx.Err() == nil {
			_ = connection.Close()
		}
	}()
	// 必须等写线程退出后才释放本轮，错误和取消都不能遗留文本生产者。
	defer func() {
		cancel()
		if ctx.Err() != nil {
			<-watchDone
		}
		_ = connection.Close()
		<-writerDone
	}()
	audioBytes := 0
	for {
		kind, pcm, err = c.readTask(streamCtx, connection, taskID)
		if err != nil {
			return err
		}
		switch kind {
		case "audio":
			if err := emit(pcm); err != nil {
				return err
			}
			audioBytes += len(pcm)
		case "task-finished":
			// 服务端不能在 finish-task 发出前声称完成；此时 writer 必须已结束。
			select {
			case <-writerDone:
				if writerErr != nil {
					return writerErr
				}
			case <-ctx.Done():
				return ctx.Err()
			case <-time.After(c.config.Timeout):
				return errors.New("CosyVoice TTS finished before text input ended")
			}
			if audioBytes == 0 {
				return errors.New("CosyVoice TTS returned no audio")
			}
			return nil
		case "result-generated":
			// 句子元信息不承载 PCM；音频在接下来的 binary 消息中。
		default:
			return errors.New("CosyVoice TTS returned an unexpected event")
		}
	}
}

func (c *ttsClient) writeText(ctx context.Context, write func(string, any) error, fragments <-chan string) error {
	synthesized := false
	filter := newTTSTextFilter()
	appendText := func(text string) error {
		if strings.TrimSpace(text) == "" {
			return nil
		}
		if err := write("continue-task", map[string]any{
			"input": map[string]any{"text": text},
		}); err != nil {
			return err
		}
		synthesized = true
		return nil
	}
	for {
		select {
		case <-ctx.Done():
			return ctx.Err()
		case fragment, ok := <-fragments:
			if !ok {
				if err := appendText(filter.Finish()); err != nil {
					return err
				}
				if !synthesized {
					return errors.New("CosyVoice TTS input text is empty")
				}
				return write("finish-task", map[string]any{"input": map[string]any{}})
			}
			if err := appendText(filter.Write(fragment)); err != nil {
				return err
			}
		}
	}
}

func (c *ttsClient) writeTask(connection *websocket.Conn, taskID, action string, payload any) error {
	if err := connection.SetWriteDeadline(time.Now().Add(c.config.Timeout)); err != nil {
		return err
	}
	return connection.WriteJSON(taskMessage(taskID, action, payload))
}

func taskMessage(taskID, action string, payload any) map[string]any {
	return map[string]any{
		"header":  map[string]any{"action": action, "task_id": taskID, "streaming": "duplex"},
		"payload": payload,
	}
}

func (c *ttsClient) readTask(ctx context.Context, connection *websocket.Conn, taskID string) (string, []byte, error) {
	if err := ctx.Err(); err != nil {
		return "", nil, err
	}
	if err := connection.SetReadDeadline(time.Now().Add(c.config.Timeout)); err != nil {
		return "", nil, err
	}
	kind, data, err := connection.ReadMessage()
	if ctx.Err() != nil {
		return "", nil, ctx.Err()
	}
	if err != nil {
		return "", nil, err
	}
	if kind == websocket.BinaryMessage {
		if len(data) == 0 || len(data)%2 != 0 || len(data) > maxTTSPCMDeltaBytes {
			return "", nil, errors.New("CosyVoice TTS returned invalid PCM")
		}
		return "audio", data, nil
	}
	var event struct {
		Header struct {
			Event     string `json:"event"`
			TaskID    string `json:"task_id"`
			ErrorCode string `json:"error_code"`
		} `json:"header"`
	}
	if kind != websocket.TextMessage || json.Unmarshal(data, &event) != nil || event.Header.TaskID != taskID {
		return "", nil, errors.New("CosyVoice TTS returned invalid task event")
	}
	if event.Header.Event == "task-failed" {
		return "", nil, fmt.Errorf("CosyVoice TTS provider_code=%q", event.Header.ErrorCode)
	}
	return event.Header.Event, nil, nil
}

func ttsRuneUnits(r rune) int {
	if r > unicode.MaxASCII {
		return 2
	}
	return 1
}

func eventID() string {
	var value [16]byte
	if _, err := rand.Read(value[:]); err != nil {
		panic("system random source unavailable")
	}
	value[6] = (value[6] & 0x0f) | 0x40
	value[8] = (value[8] & 0x3f) | 0x80
	encoded := hex.EncodeToString(value[:])
	return encoded[:8] + "-" + encoded[8:12] + "-" + encoded[12:16] + "-" + encoded[16:20] + "-" + encoded[20:]
}
