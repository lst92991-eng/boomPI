package protocol

import (
	"bytes"
	"encoding/hex"
	"encoding/json"
	"os"
	"strings"
	"testing"
)

func TestSharedV4GoldenFixtures(t *testing.T) {
	data, err := os.ReadFile("../../../protocol/fixtures/protocol-v4-golden.json")
	if err != nil {
		t.Fatal(err)
	}
	var fixtures struct {
		Controls []struct {
			Name     string
			Wire     string `json:"wire_text"`
			Expected json.RawMessage
		} `json:"control_frames"`
		Audio []struct {
			Name, Direction string
			Wire            string `json:"wire_hex"`
			Header          PCMHeader
		} `json:"audio_frames"`
		Invalid []string `json:"invalid_control_frames"`
	}
	if err = json.Unmarshal(data, &fixtures); err != nil {
		t.Fatal(err)
	}
	for _, fixture := range fixtures.Controls {
		t.Run(fixture.Name, func(t *testing.T) {
			control, err := DecodeControl([]byte(fixture.Wire))
			if err != nil {
				t.Fatal(err)
			}
			wire, err := EncodeControl(control)
			if err != nil {
				t.Fatal(err)
			}
			var got, want any
			encoded, _ := json.Marshal(control)
			_ = json.Unmarshal(encoded, &got)
			if string(wire) != fixture.Wire {
				t.Fatalf("wire round trip mismatch: %q", wire)
			}
			_ = json.Unmarshal(fixture.Expected, &want)
			g, _ := json.Marshal(got)
			w, _ := json.Marshal(want)
			if !bytes.Equal(g, w) {
				t.Fatalf("got %s, want %s", g, w)
			}
		})
	}
	for _, fixture := range fixtures.Audio {
		t.Run(fixture.Name, func(t *testing.T) {
			wire, err := hex.DecodeString(fixture.Wire)
			if err != nil {
				t.Fatal(err)
			}
			up := fixture.Direction == "uplink"
			header, pcm, err := ParsePCMFrame(wire, up)
			if err != nil {
				t.Fatal(err)
			}
			if header != fixture.Header {
				t.Fatalf("header %+v, want %+v", header, fixture.Header)
			}
			encoded, err := EncodePCM(header, pcm, up)
			if err != nil || !bytes.Equal(encoded, wire) {
				t.Fatalf("round trip %v", err)
			}
		})
	}
	for _, wire := range fixtures.Invalid {
		if _, err := DecodeControl([]byte(wire)); err == nil {
			t.Errorf("accepted %s", wire)
		}
	}
}

func TestControlLimitsAndTypes(t *testing.T) {
	for _, data := range [][]byte{
		[]byte("[]"), []byte("{}"), []byte("null"),
		[]byte("{\"type\":\"text\",\"generation\":1,\"text\":\"\xff\"}"),
		[]byte("{\"type\":\"stop\",\"generation\":2,\"retract\":0}"),
		[]byte("DONE -1"),
		[]byte("DONE 01"),
		[]byte("ERROR 1 Raw provider secret!"),
	} {
		if _, err := DecodeControl(data); err == nil {
			t.Errorf("accepted %q", data)
		}
	}
	for _, n := range []int{4096, 4097} {
		_, err := EncodeControl(Control{Type: "text", Generation: 1, Text: strings.Repeat("x", n)})
		if (err == nil) != (n == 4096) {
			t.Fatalf("text length %d: %v", n, err)
		}
	}
}

func TestUTF8TextIsDataNotEscapedControl(t *testing.T) {
	for _, tc := range []struct {
		value string
		valid bool
	}{
		{"你好", true}, {"😀", true}, {"line 1\nline 2", true}, {`\ud800`, true},
		{string([]byte{0xc3, 0x28}), false}, {string([]byte{0}), false},
	} {
		_, err := DecodeControl([]byte("TEXT 1 " + tc.value))
		if (err == nil) != tc.valid {
			t.Errorf("%q: %v", tc.value, err)
		}
	}
}

func TestPCMRejectsMalformedFrames(t *testing.T) {
	valid, _ := EncodePCM(PCMHeader{Generation: 1}, make([]byte, 640), true)
	for _, mutate := range []func([]byte) []byte{
		func(b []byte) []byte { return b[:11] },
		func(b []byte) []byte { b[3] = '2'; return b },
		func(b []byte) []byte { b[3] = '3'; return b },
		func(b []byte) []byte { clear(b[4:8]); return b },
		func(b []byte) []byte {
			for i := 8; i < 12; i++ {
				b[i] = 255
			}
			return b
		},
		func(b []byte) []byte { return b[:len(b)-2] },
	} {
		if _, _, err := ParsePCMFrame(mutate(append([]byte(nil), valid...)), true); err == nil {
			t.Fatal("accepted malformed PCM")
		}
	}
	for _, size := range []int{0, 1, 2, 3, 638, 640, 642} {
		_, err := EncodePCM(PCMHeader{Generation: 1}, make([]byte, size), false)
		valid := size > 0 && size <= 640 && size%2 == 0
		if (err == nil) != valid {
			t.Errorf("size %d: %v", size, err)
		}
	}
}

func FuzzDecodeV4(f *testing.F) {
	f.Add([]byte("{\"type\":\"ready\"}"))
	f.Add([]byte("BPV4"))
	f.Fuzz(func(t *testing.T, data []byte) {
		_, _ = DecodeControl(data)
		_, _, _ = ParsePCMFrame(data, true)
		_, _, _ = ParsePCMFrame(data, false)
	})
}
