package qwenpipeline

import (
	"bytes"
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

func TestContinuousTTSSessionUsesLowLatencyCommitAndFinish(t *testing.T) {
	serverResult := make(chan error, 1)
	server := httptest.NewServer(http.HandlerFunc(func(response http.ResponseWriter, request *http.Request) {
		serverResult <- serveContinuousTTSSession(response, request)
	}))
	defer server.Close()

	connection, _, err := websocket.DefaultDialer.Dial(
		strings.Replace(server.URL, "http://", "ws://", 1), nil)
	if err != nil {
		t.Fatalf("dial test TTS server: %v", err)
	}
	defer connection.Close()

	fragments := make(chan string, 2)
	fragments <- "第一句 **正确**。"
	fragments <- "第二句。"
	close(fragments)
	client := &ttsClient{config: Config{TTSVoice: "Cherry", Timeout: 2 * time.Second}}
	var audio []byte
	ctx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
	defer cancel()
	if err := client.synthesizeConnectedStream(ctx, connection, fragments, func(pcm []byte) error {
		audio = append(audio, pcm...)
		return nil
	}); err != nil {
		t.Fatalf("synthesizeConnectedStream: %v", err)
	}
	if err := <-serverResult; err != nil {
		t.Fatal(err)
	}
	if want := []byte{1, 2, 3, 4}; !bytes.Equal(audio, want) {
		t.Fatalf("emitted PCM = %v, want %v", audio, want)
	}
}

func TestContinuousTTSSessionRequiresCompletedResponseAndAudio(t *testing.T) {
	testCases := []struct {
		name          string
		sendAudio     bool
		sendCompleted bool
		wantError     string
	}{
		{
			name: "audio_without_completed_response", sendAudio: true,
			wantError: "without a completed response",
		},
		{
			name: "completed_response_without_audio", sendCompleted: true,
			wantError: "without PCM audio",
		},
	}
	for _, testCase := range testCases {
		t.Run(testCase.name, func(t *testing.T) {
			serverResult := make(chan error, 1)
			server := httptest.NewServer(http.HandlerFunc(func(response http.ResponseWriter, request *http.Request) {
				serverResult <- serveIncompleteTTSSession(
					response, request, testCase.sendAudio, testCase.sendCompleted)
			}))
			defer server.Close()

			connection, _, err := websocket.DefaultDialer.Dial(
				strings.Replace(server.URL, "http://", "ws://", 1), nil)
			if err != nil {
				t.Fatalf("dial test TTS server: %v", err)
			}
			defer connection.Close()

			fragments := make(chan string, 1)
			fragments <- "测试"
			close(fragments)
			client := &ttsClient{config: Config{TTSVoice: "Cherry", Timeout: 2 * time.Second}}
			ctx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
			defer cancel()
			err = client.synthesizeConnectedStream(ctx, connection, fragments, func([]byte) error {
				return nil
			})
			if err == nil || !strings.Contains(err.Error(), testCase.wantError) {
				t.Fatalf("synthesizeConnectedStream() error = %v, want %q", err, testCase.wantError)
			}
			if serverErr := <-serverResult; serverErr != nil {
				t.Fatal(serverErr)
			}
		})
	}
}

func serveContinuousTTSSession(response http.ResponseWriter, request *http.Request) error {
	connection, err := (&websocket.Upgrader{}).Upgrade(response, request, nil)
	if err != nil {
		return fmt.Errorf("upgrade test TTS connection: %w", err)
	}
	defer connection.Close()
	if err := connection.WriteJSON(map[string]any{"type": "session.created"}); err != nil {
		return err
	}
	var update struct {
		Type    string `json:"type"`
		Session struct {
			Mode           string `json:"mode"`
			ResponseFormat string `json:"response_format"`
			SampleRate     int    `json:"sample_rate"`
		} `json:"session"`
	}
	if err := connection.ReadJSON(&update); err != nil {
		return err
	}
	if update.Type != "session.update" || update.Session.Mode != "commit" ||
		update.Session.ResponseFormat != "pcm" || update.Session.SampleRate != 24000 {
		return fmt.Errorf("unexpected session.update: %+v", update)
	}
	if err := connection.WriteJSON(map[string]any{"type": "session.updated"}); err != nil {
		return err
	}

	for index, wantText := range []string{"第一句 正确。", "第二句。"} {
		var appendEvent struct {
			Type string `json:"type"`
			Text string `json:"text"`
		}
		if err := connection.ReadJSON(&appendEvent); err != nil {
			return err
		}
		if appendEvent.Type != "input_text_buffer.append" || appendEvent.Text != wantText {
			return fmt.Errorf("append event %d = %+v", index, appendEvent)
		}
		var commitEvent struct {
			Type string `json:"type"`
		}
		if err := connection.ReadJSON(&commitEvent); err != nil {
			return err
		}
		if commitEvent.Type != "input_text_buffer.commit" {
			return fmt.Errorf("commit event %d = %+v", index, commitEvent)
		}
		pcm := []byte{byte(index*2 + 1), byte(index*2 + 2)}
		if err := connection.WriteJSON(map[string]any{
			"type": "response.audio.delta", "delta": base64.StdEncoding.EncodeToString(pcm),
		}); err != nil {
			return err
		}
		if err := connection.WriteJSON(map[string]any{
			"type": "response.done", "response": map[string]any{"status": "completed"},
		}); err != nil {
			return err
		}
	}
	var finish struct {
		Type string `json:"type"`
	}
	if err := connection.ReadJSON(&finish); err != nil {
		return err
	}
	if finish.Type != "session.finish" {
		return fmt.Errorf("terminal client event = %q, want session.finish", finish.Type)
	}
	return connection.WriteJSON(map[string]any{"type": "session.finished"})
}

func serveIncompleteTTSSession(
	response http.ResponseWriter,
	request *http.Request,
	sendAudio bool,
	sendCompleted bool,
) error {
	connection, err := (&websocket.Upgrader{}).Upgrade(response, request, nil)
	if err != nil {
		return fmt.Errorf("upgrade test TTS connection: %w", err)
	}
	defer connection.Close()
	if err := connection.WriteJSON(map[string]any{"type": "session.created"}); err != nil {
		return err
	}
	var update struct {
		Type string `json:"type"`
	}
	if err := connection.ReadJSON(&update); err != nil {
		return err
	}
	if update.Type != "session.update" {
		return fmt.Errorf("initial client event = %q, want session.update", update.Type)
	}
	if err := connection.WriteJSON(map[string]any{"type": "session.updated"}); err != nil {
		return err
	}
	for _, wantType := range []string{"input_text_buffer.append", "input_text_buffer.commit", "session.finish"} {
		var event struct {
			Type string `json:"type"`
		}
		if err := connection.ReadJSON(&event); err != nil {
			return err
		}
		if event.Type != wantType {
			return fmt.Errorf("client event = %q, want %q", event.Type, wantType)
		}
	}
	if sendAudio {
		if err := connection.WriteJSON(map[string]any{
			"type":  "response.audio.delta",
			"delta": base64.StdEncoding.EncodeToString([]byte{1, 2}),
		}); err != nil {
			return err
		}
	}
	if sendCompleted {
		if err := connection.WriteJSON(map[string]any{
			"type": "response.done", "response": map[string]any{"status": "completed"},
		}); err != nil {
			return err
		}
	}
	return connection.WriteJSON(map[string]any{"type": "session.finished"})
}

func TestSplitTTSFragmentsKeepsTextAndLimit(t *testing.T) {
	input := strings.Repeat("这是一句很长的中文。", 300) + strings.Repeat("ascii text. ", 100)
	fragments := splitTTSFragments(input, maxTTSFragmentUnits)
	if len(fragments) < 2 {
		t.Fatalf("expected multiple fragments, got %d", len(fragments))
	}
	for index, fragment := range fragments {
		if units := ttsTextUnits(fragment); units > maxTTSFragmentUnits {
			t.Fatalf("fragment %d has %d units", index, units)
		}
	}
	if got, want := strings.Join(fragments, ""), strings.TrimSpace(input); got != want {
		t.Fatal("fragmentation changed input text")
	}
}

func TestSplitTTSFragmentsRejectsEmptyInput(t *testing.T) {
	if fragments := splitTTSFragments(" \n\t", maxTTSFragmentUnits); fragments != nil {
		t.Fatalf("expected no fragments, got %#v", fragments)
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
		if units := ttsTextUnits(fragment); units > maxTTSFragmentUnits {
			t.Fatalf("fragment %d has %d units", index, units)
		}
	}
	const whitespace = " \n\t "
	if fragments := streamingTTSFragments(whitespace); len(fragments) != 1 || fragments[0] != whitespace {
		t.Fatalf("whitespace-only delta fragments = %#v, want preserved whitespace", fragments)
	}
}
