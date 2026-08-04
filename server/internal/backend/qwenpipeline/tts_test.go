package qwenpipeline

import (
	"bytes"
	"context"
	"encoding/base64"
	"errors"
	"fmt"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"

	"github.com/gorilla/websocket"
)

func TestContinuousTTSSessionUsesServerCommitAndFinish(t *testing.T) {
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

func TestContinuousTTSSessionStreamsPCMBeforeInputCloses(t *testing.T) {
	serverResult := make(chan error, 1)
	server := httptest.NewServer(http.HandlerFunc(func(response http.ResponseWriter, request *http.Request) {
		serverResult <- serveStreamingTTSSession(response, request)
	}))
	defer server.Close()

	connection, _, err := websocket.DefaultDialer.Dial(
		strings.Replace(server.URL, "http://", "ws://", 1), nil)
	if err != nil {
		t.Fatalf("dial test TTS server: %v", err)
	}
	defer connection.Close()

	// Keep the channel open after the first fragment. Receiving PCM before the
	// second send proves that neither channel close nor session.finish gates TTS.
	fragments := make(chan string)
	audioReceived := make(chan []byte, 2)
	clientResult := make(chan error, 1)
	client := &ttsClient{config: Config{TTSVoice: "Cherry", Timeout: time.Second}}
	ctx, cancel := context.WithTimeout(context.Background(), 2*time.Second)
	defer cancel()
	go func() {
		clientResult <- client.synthesizeConnectedStream(
			ctx, connection, fragments,
			func(pcm []byte) error {
				audioReceived <- append([]byte(nil), pcm...)
				return nil
			},
		)
	}()

	fragments <- "first sentence."
	select {
	case pcm := <-audioReceived:
		if want := []byte{11, 12}; !bytes.Equal(pcm, want) {
			t.Fatalf("first PCM = %v, want %v", pcm, want)
		}
	case <-time.After(500 * time.Millisecond):
		t.Fatal("first PCM waited for a second fragment or input close")
	}

	fragments <- "second sentence."
	close(fragments)
	select {
	case err := <-clientResult:
		if err != nil {
			t.Fatalf("synthesizeConnectedStream: %v", err)
		}
	case <-time.After(time.Second):
		t.Fatal("TTS client did not finish")
	}
	if err := <-serverResult; err != nil {
		t.Fatal(err)
	}
}

func TestContinuousTTSSessionCancellationUnblocksOpenInput(t *testing.T) {
	serverReady := make(chan struct{})
	serverResult := make(chan error, 1)
	server := httptest.NewServer(http.HandlerFunc(func(response http.ResponseWriter, request *http.Request) {
		serverResult <- serveBlockedTTSSession(response, request, serverReady)
	}))
	defer server.Close()

	connection, _, err := websocket.DefaultDialer.Dial(
		strings.Replace(server.URL, "http://", "ws://", 1), nil)
	if err != nil {
		t.Fatalf("dial test TTS server: %v", err)
	}
	defer connection.Close()

	fragments := make(chan string)
	client := &ttsClient{config: Config{TTSVoice: "Cherry", Timeout: 2 * time.Second}}
	ctx, cancel := context.WithCancel(context.Background())
	clientResult := make(chan error, 1)
	go func() {
		clientResult <- client.synthesizeConnectedStream(
			ctx, connection, fragments, func([]byte) error { return nil })
	}()

	select {
	case <-serverReady:
	case <-time.After(time.Second):
		t.Fatal("TTS session did not reach the blocked streaming phase")
	}
	cancelStarted := time.Now()
	cancel()
	select {
	case err := <-clientResult:
		if !errors.Is(err, context.Canceled) {
			t.Fatalf("canceled synthesizeConnectedStream error = %v, want context.Canceled", err)
		}
		if elapsed := time.Since(cancelStarted); elapsed > 500*time.Millisecond {
			t.Fatalf("canceled TTS session took %s, want at most 500ms", elapsed)
		}
	case <-time.After(500 * time.Millisecond):
		t.Fatal("canceled TTS session left its reader or writer blocked")
	}
	select {
	case err := <-serverResult:
		if err != nil {
			t.Fatal(err)
		}
	case <-time.After(500 * time.Millisecond):
		t.Fatal("canceled TTS session did not close the WebSocket")
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
	if update.Type != "session.update" || update.Session.Mode != "server_commit" ||
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

func serveStreamingTTSSession(response http.ResponseWriter, request *http.Request) error {
	connection, err := (&websocket.Upgrader{}).Upgrade(response, request, nil)
	if err != nil {
		return fmt.Errorf("upgrade streaming TTS connection: %w", err)
	}
	defer connection.Close()
	if err := completeTestTTSSessionHandshake(connection); err != nil {
		return err
	}

	for index, wantText := range []string{"first sentence.", "second sentence."} {
		var event struct {
			Type string `json:"type"`
			Text string `json:"text"`
		}
		if err := connection.ReadJSON(&event); err != nil {
			return err
		}
		if event.Type != "input_text_buffer.append" || event.Text != wantText {
			return fmt.Errorf("streaming append %d = %+v", index, event)
		}
		pcm := []byte{byte(11 + index*2), byte(12 + index*2)}
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

func serveBlockedTTSSession(
	response http.ResponseWriter,
	request *http.Request,
	ready chan<- struct{},
) error {
	connection, err := (&websocket.Upgrader{}).Upgrade(response, request, nil)
	if err != nil {
		return fmt.Errorf("upgrade blocked TTS connection: %w", err)
	}
	defer connection.Close()
	if err := completeTestTTSSessionHandshake(connection); err != nil {
		return err
	}
	close(ready)
	if _, _, err := connection.ReadMessage(); err == nil {
		return errors.New("blocked TTS session received an event before cancellation")
	}
	return nil
}

func completeTestTTSSessionHandshake(connection *websocket.Conn) error {
	if err := connection.WriteJSON(map[string]any{"type": "session.created"}); err != nil {
		return err
	}
	var update struct {
		Type    string `json:"type"`
		Session struct {
			Mode string `json:"mode"`
		} `json:"session"`
	}
	if err := connection.ReadJSON(&update); err != nil {
		return err
	}
	if update.Type != "session.update" || update.Session.Mode != "server_commit" {
		return fmt.Errorf("unexpected session.update: %+v", update)
	}
	return connection.WriteJSON(map[string]any{"type": "session.updated"})
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
	// server_commit accepts streamed append events and must not receive an
	// input_text_buffer.commit from the client.
	for _, wantType := range []string{"input_text_buffer.append", "session.finish"} {
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
