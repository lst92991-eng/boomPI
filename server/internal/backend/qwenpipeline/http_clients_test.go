package qwenpipeline

import (
	"context"
	"errors"
	"io"
	"strings"
	"testing"
	"time"
)

func TestReadResponsesStream(t *testing.T) {
	input := strings.Join([]string{
		`event: response.output_text.delta`,
		`data: {"type":"response.output_text.delta","delta":"你好"}`,
		``,
		`data: {"type":"response.output_text.delta","delta":"，世界"}`,
		``,
		`data: {"type":"response.completed","response":{"status":"completed"}}`,
		``,
	}, "\n")
	var deltas []string
	answer, err := readResponsesStream(strings.NewReader(input), func(delta string) error {
		deltas = append(deltas, delta)
		return nil
	})
	if err != nil {
		t.Fatal(err)
	}
	if answer != "你好，世界" {
		t.Fatalf("unexpected answer %q", answer)
	}
	if got := strings.Join(deltas, ""); got != answer {
		t.Fatalf("unexpected deltas %q", got)
	}
}

func TestResponsesStreamTimesOutOnlyWhenProviderStopsProgress(t *testing.T) {
	reader, writer := io.Pipe()
	defer writer.Close()
	started := time.Now()
	_, err := readResponsesStreamWithTimeout(
		context.Background(), reader, 30*time.Millisecond, nil,
	)
	if !errors.Is(err, context.DeadlineExceeded) {
		t.Fatalf("readResponsesStreamWithTimeout() error = %v, want deadline", err)
	}
	if elapsed := time.Since(started); elapsed > 250*time.Millisecond {
		t.Fatalf("provider stall took %s to abort", elapsed)
	}
}

func TestResponsesStreamDoesNotTimeoutDuringDownstreamBackpressure(t *testing.T) {
	input := strings.Join([]string{
		`data: {"type":"response.output_text.delta","delta":"hello"}`,
		`data: {"type":"response.completed","response":{"status":"completed"}}`,
		``,
	}, "\n")
	body := io.NopCloser(strings.NewReader(input))
	answer, err := readResponsesStreamWithTimeout(
		context.Background(), body, 30*time.Millisecond,
		func(string) error {
			time.Sleep(60 * time.Millisecond)
			return nil
		},
	)
	if err != nil || answer != "hello" {
		t.Fatalf("backpressured stream = %q, %v", answer, err)
	}
}

func TestReadResponsesStreamRequiresCompletedEvent(t *testing.T) {
	_, err := readResponsesStream(strings.NewReader(
		`data: {"type":"response.output_text.delta","delta":"partial"}`+"\n",
	), nil)
	if err == nil || !strings.Contains(err.Error(), "before response.completed") {
		t.Fatalf("unexpected error %v", err)
	}
}
