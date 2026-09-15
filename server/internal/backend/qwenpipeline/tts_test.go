package qwenpipeline

import (
	"bytes"
	"context"
	"errors"
	"fmt"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"

	"github.com/gorilla/websocket"
)

type ttsRequest struct {
	Header struct {
		Action    string `json:"action"`
		TaskID    string `json:"task_id"`
		Streaming string `json:"streaming"`
	} `json:"header"`
	Payload struct {
		Model      string `json:"model"`
		Parameters struct {
			Voice      string `json:"voice"`
			Format     string `json:"format"`
			SampleRate int    `json:"sample_rate"`
		} `json:"parameters"`
		Input struct {
			Text      string `json:"text"`
			Directive string `json:"directive"`
		} `json:"input"`
	} `json:"payload"`
}

func openTestTTS(t *testing.T, serve func(*websocket.Conn, string)) (*ttsClient, *websocket.Conn) {
	t.Helper()
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		conn, err := (&websocket.Upgrader{}).Upgrade(w, r, nil)
		if err != nil {
			t.Error(err)
			return
		}
		defer conn.Close()
		var run ttsRequest
		if err := conn.ReadJSON(&run); err != nil {
			t.Error(err)
			return
		}
		if run.Header.Action != "run-task" || run.Header.Streaming != "duplex" || len(run.Header.TaskID) != 36 ||
			run.Payload.Model != "cosyvoice-v3-flash" || run.Payload.Parameters.Voice != "longxiaochun_v3" ||
			run.Payload.Parameters.Format != "pcm" || run.Payload.Parameters.SampleRate != 16000 {
			t.Errorf("invalid synthesis request: %+v", run)
			return
		}
		_ = conn.WriteJSON(map[string]any{"header": map[string]any{"event": "task-started", "task_id": run.Header.TaskID}})
		serve(conn, run.Header.TaskID)
	}))
	t.Cleanup(server.Close)
	conn, _, err := websocket.DefaultDialer.Dial(strings.Replace(server.URL, "http://", "ws://", 1), nil)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { conn.Close() })
	return &ttsClient{config: Config{TTSModel: "cosyvoice-v3-flash", TTSVoice: "longxiaochun_v3", Timeout: time.Second}}, conn
}

func TestCosyVoiceStreamsBeforeInputClosesAndFinishesTail(t *testing.T) {
	serverDone := make(chan struct{})
	client, conn := openTestTTS(t, func(conn *websocket.Conn, id string) {
		defer close(serverDone)
		for index, text := range []string{"第一句 正确。", "第二句。"} {
			var request ttsRequest
			if err := conn.ReadJSON(&request); err != nil {
				t.Error(err)
				return
			}
			if request.Header.Action != "continue-task" || request.Header.TaskID != id || request.Payload.Input.Text != text {
				t.Errorf("text %d: %+v", index, request)
				return
			}
			_ = conn.WriteMessage(websocket.BinaryMessage, []byte{byte(index + 1), 0})
		}
		var finish ttsRequest
		if err := conn.ReadJSON(&finish); err != nil {
			t.Error(err)
			return
		}
		if finish.Header.Action != "finish-task" || finish.Header.TaskID != id {
			t.Errorf("finish: %+v", finish)
			return
		}
		_ = conn.WriteMessage(websocket.BinaryMessage, []byte{3, 0})
		_ = conn.WriteJSON(map[string]any{"header": map[string]any{"event": "task-finished", "task_id": id}})
	})
	fragments := make(chan string)
	audio := make(chan []byte, 3)
	result := make(chan error, 1)
	ctx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
	defer cancel()
	go func() {
		result <- client.synthesizeConnectedStream(ctx, conn, fragments, func(p []byte) error { audio <- p; return nil })
	}()
	fragments <- "第一句 **正确**。"
	select {
	case p := <-audio:
		if !bytes.Equal(p, []byte{1, 0}) {
			t.Fatal(p)
		}
	case <-ctx.Done():
		t.Fatal("first PCM waited for END")
	}
	fragments <- "第二句。"
	close(fragments)
	if err := <-result; err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(<-audio, []byte{2, 0}) || !bytes.Equal(<-audio, []byte{3, 0}) {
		t.Fatal("audio tail lost")
	}
	<-serverDone
}

func TestCosyVoiceCancellationClosesCloudAndJoinsWriter(t *testing.T) {
	for _, afterEnd := range []bool{false, true} {
		t.Run(fmt.Sprint("after_end=", afterEnd), func(t *testing.T) {
			ready, closed := make(chan struct{}), make(chan struct{})
			client, conn := openTestTTS(t, func(conn *websocket.Conn, id string) {
				if afterEnd {
					for i := 0; i < 2; i++ {
						var req ttsRequest
						if conn.ReadJSON(&req) != nil {
							return
						}
					}
				}
				close(ready)
				var req ttsRequest
				if err := conn.ReadJSON(&req); err != nil {
					t.Errorf("missing cancel directive: %v", err)
				} else if req.Header.TaskID != id || req.Header.Action != "finish-task" || req.Payload.Input.Directive != "cancel" {
					t.Errorf("cancel request: %+v", req)
				}
				_, _, _ = conn.ReadMessage()
				close(closed)
			})
			ctx, cancel := context.WithCancel(context.Background())
			defer cancel()
			fragments := make(chan string, 1)
			if afterEnd {
				fragments <- "测试。"
				close(fragments)
			}
			result := make(chan error, 1)
			go func() {
				result <- client.synthesizeConnectedStream(ctx, conn, fragments, func([]byte) error { t.Error("unexpected audio"); return nil })
			}()
			<-ready
			cancel()
			select {
			case err := <-result:
				if !errors.Is(err, context.Canceled) {
					t.Fatalf("cancel: %v", err)
				}
			case <-time.After(500 * time.Millisecond):
				t.Fatal("cancel blocked")
			}
			select {
			case <-closed:
			case <-time.After(time.Second):
				t.Fatal("cloud connection survived cancel")
			}
		})
	}
}

func TestCosyVoiceRejectsInvalidOrIncompleteOutput(t *testing.T) {
	for _, scenario := range []string{"odd_pcm", "oversized_pcm", "wrong_task", "no_audio", "missing_finish", "provider_error"} {
		t.Run(scenario, func(t *testing.T) {
			client, conn := openTestTTS(t, func(conn *websocket.Conn, id string) {
				for i := 0; i < 2; i++ {
					var req ttsRequest
					if conn.ReadJSON(&req) != nil {
						return
					}
				}
				switch scenario {
				case "odd_pcm":
					_ = conn.WriteMessage(websocket.BinaryMessage, []byte{1})
				case "oversized_pcm":
					_ = conn.WriteMessage(websocket.BinaryMessage, make([]byte, maxTTSPCMDeltaBytes+2))
				case "wrong_task":
					_ = conn.WriteJSON(map[string]any{"header": map[string]any{"event": "task-finished", "task_id": "old"}})
				case "no_audio":
					_ = conn.WriteJSON(map[string]any{"header": map[string]any{"event": "task-finished", "task_id": id}})
				case "missing_finish":
					_ = conn.WriteMessage(websocket.BinaryMessage, []byte{1, 0})
				case "provider_error":
					_ = conn.WriteJSON(map[string]any{"header": map[string]any{"event": "task-failed", "task_id": id, "error_code": "InvalidParameter"}})
				}
			})
			fragments := make(chan string, 1)
			fragments <- "测试。"
			close(fragments)
			ctx, cancel := context.WithTimeout(context.Background(), 2*time.Second)
			defer cancel()
			if err := client.synthesizeConnectedStream(ctx, conn, fragments, func([]byte) error { return nil }); err == nil {
				t.Fatal("accepted invalid output")
			}
		})
	}
}

func TestStreamingTTSFragmentsSendFirstDeltaImmediately(t *testing.T) {
	const delta = "你好"
	fragments := streamingTTSFragments(delta)
	if len(fragments) != 1 || fragments[0] != delta {
		t.Fatalf("first delta fragments = %#v, want immediate %q", fragments, delta)
	}
}

func TestStreamingTTSFragmentsPreserveTextAndBoundSize(t *testing.T) {
	delta := "  " + strings.Repeat("中", maxTTSFragmentUnits) + " tail "
	fragments := streamingTTSFragments(delta)
	if got := strings.Join(fragments, ""); got != delta {
		t.Fatalf("streaming fragments changed text: got %q", got)
	}
	if len(fragments) < 2 {
		t.Fatalf("expected a bounded split, got %#v", fragments)
	}
	for index, fragment := range fragments {
		units := 0
		for _, current := range fragment {
			units += ttsRuneUnits(current)
		}
		if units > maxTTSFragmentUnits {
			t.Fatalf("fragment %d has %d units", index, units)
		}
	}
	const whitespace = " \n\t "
	if fragments := streamingTTSFragments(whitespace); len(fragments) != 1 || fragments[0] != whitespace {
		t.Fatalf("whitespace-only delta fragments = %#v, want preserved whitespace", fragments)
	}
}
