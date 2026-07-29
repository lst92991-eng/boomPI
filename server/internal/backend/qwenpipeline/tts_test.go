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
	fragments <- "第一句。"
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

	for index, wantText := range []string{"第一句。", "第二句。"} {
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

func TestStreamingTTSSegmenterEmitsCompleteSentence(t *testing.T) {
	segmenter := &streamingTTSSegmenter{}
	if got := segmenter.Add("这是第一段，还没说完"); len(got) != 0 {
		t.Fatalf("unexpected early fragments %#v", got)
	}
	got := segmenter.Add("，现在用一个完整句子结束。下一句")
	if len(got) != 1 || !strings.HasSuffix(got[0], "。") {
		t.Fatalf("expected one complete sentence, got %#v", got)
	}
	if tail := segmenter.Flush(); tail != "下一句" {
		t.Fatalf("unexpected tail %q", tail)
	}
}
