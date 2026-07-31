package qwenpipeline

import (
	"context"
	"encoding/base64"
	"fmt"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"

	"github.com/gorilla/websocket"
)

func TestRealtimeASRStreamsAudioBeforeCommit(t *testing.T) {
	serverResult := make(chan error, 1)
	server := httptest.NewServer(http.HandlerFunc(func(response http.ResponseWriter, request *http.Request) {
		serverResult <- serveRealtimeASR(response, request)
	}))
	defer server.Close()

	config := Config{APIKey: "test-key", Timeout: 2 * time.Second}
	ctx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
	defer cancel()
	stream, err := openRealtimeASRAt(ctx, config,
		strings.Replace(server.URL, "http://", "ws://", 1))
	if err != nil {
		t.Fatalf("open realtime ASR: %v", err)
	}
	defer stream.Close()
	if err := stream.Append([]byte{1, 2, 3, 4}); err != nil {
		t.Fatalf("append first PCM: %v", err)
	}
	if err := stream.Append([]byte{5, 6, 7, 8}); err != nil {
		t.Fatalf("append second PCM: %v", err)
	}
	transcript, err := stream.Commit(ctx)
	if err != nil {
		t.Fatalf("commit realtime ASR: %v", err)
	}
	if transcript != "测试成功" {
		t.Fatalf("transcript = %q", transcript)
	}
	if err := <-serverResult; err != nil {
		t.Fatal(err)
	}
}

func serveRealtimeASR(response http.ResponseWriter, request *http.Request) error {
	if request.Header.Get("Authorization") != "Bearer test-key" ||
		request.Header.Get("OpenAI-Beta") != "realtime=v1" {
		return fmt.Errorf("unexpected authentication headers")
	}
	connection, err := (&websocket.Upgrader{}).Upgrade(response, request, nil)
	if err != nil {
		return err
	}
	defer connection.Close()
	if err := connection.WriteJSON(map[string]any{"type": "session.created"}); err != nil {
		return err
	}
	var update struct {
		Type    string `json:"type"`
		Session struct {
			Format        string `json:"input_audio_format"`
			SampleRate    int    `json:"sample_rate"`
			TurnDetection any    `json:"turn_detection"`
		} `json:"session"`
	}
	if err := connection.ReadJSON(&update); err != nil {
		return err
	}
	if update.Type != "session.update" || update.Session.Format != "pcm" ||
		update.Session.SampleRate != 16000 || update.Session.TurnDetection != nil {
		return fmt.Errorf("unexpected session update: %+v", update)
	}
	if err := connection.WriteJSON(map[string]any{"type": "session.updated"}); err != nil {
		return err
	}

	wantPCM := [][]byte{{1, 2, 3, 4}, {5, 6, 7, 8}}
	for index, want := range wantPCM {
		var appendEvent struct {
			Type  string `json:"type"`
			Audio string `json:"audio"`
		}
		if err := connection.ReadJSON(&appendEvent); err != nil {
			return err
		}
		pcm, err := base64.StdEncoding.DecodeString(appendEvent.Audio)
		if err != nil || appendEvent.Type != "input_audio_buffer.append" ||
			string(pcm) != string(want) {
			return fmt.Errorf("append event %d is invalid", index)
		}
	}
	for _, wanted := range []string{"input_audio_buffer.commit", "session.finish"} {
		var event struct {
			Type string `json:"type"`
		}
		if err := connection.ReadJSON(&event); err != nil {
			return err
		}
		if event.Type != wanted {
			return fmt.Errorf("event type = %q, want %q", event.Type, wanted)
		}
	}
	if err := connection.WriteJSON(map[string]any{
		"type": "input_audio_buffer.committed",
	}); err != nil {
		return err
	}
	if err := connection.WriteJSON(map[string]any{
		"type":       "conversation.item.input_audio_transcription.completed",
		"transcript": "测试成功",
	}); err != nil {
		return err
	}
	return connection.WriteJSON(map[string]any{"type": "session.finished"})
}
