package qwenpipeline

import (
	"strings"
	"testing"
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

func TestReadResponsesStreamRequiresCompletedEvent(t *testing.T) {
	_, err := readResponsesStream(strings.NewReader(
		`data: {"type":"response.output_text.delta","delta":"partial"}`+"\n",
	), nil)
	if err == nil || !strings.Contains(err.Error(), "before response.completed") {
		t.Fatalf("unexpected error %v", err)
	}
}
