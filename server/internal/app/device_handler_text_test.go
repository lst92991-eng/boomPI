package app

import (
	"strings"
	"testing"
	"unicode/utf8"
)

func TestSplitTextDeltaKeepsUTF8AndProtocolLimit(t *testing.T) {
	original := strings.Repeat("你好🙂boomPI", 900)
	chunks, err := splitTextDelta(original)
	if err != nil {
		t.Fatalf("splitTextDelta() error = %v", err)
	}
	if len(chunks) < 2 {
		t.Fatal("large provider delta was not split")
	}
	for index, chunk := range chunks {
		if len(chunk) == 0 || len(chunk) > maxTextDeltaBytes || !utf8.ValidString(chunk) {
			t.Fatalf("chunk %d has invalid UTF-8/length: %d", index, len(chunk))
		}
	}
	if joined := strings.Join(chunks, ""); joined != original {
		t.Fatal("splitTextDelta changed the provider text")
	}
}

func TestSplitTextDeltaRejectsInvalidInput(t *testing.T) {
	for _, text := range []string{"", string([]byte{0xff})} {
		if _, err := splitTextDelta(text); err == nil {
			t.Fatal("splitTextDelta accepted invalid input")
		}
	}
}
