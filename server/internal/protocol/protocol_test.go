package protocol

import (
	"bytes"
	"encoding/hex"
	"encoding/json"
	"os"
	"strings"
	"testing"
)

func TestSharedV2GoldenFixtures(t *testing.T) {
	data, err := os.ReadFile("../../../protocol/fixtures/protocol-v2-golden.json")
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
			_ = json.Unmarshal(wire, &got)
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
		[]byte("{\"type\":\"done\",\"generation\":-1}"),
		[]byte("{\"type\":\"done\",\"generation\":\"1\"}"),
		[]byte("{\"type\":\"error\",\"generation\":1,\"code\":\"Raw provider secret!\"}"),
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

func TestUnicodeEscapesAgreeWithBoard(t *testing.T) {
	for _, tc := range []struct {
		value string
		valid bool
	}{
		{`\ud800`, false}, {`\udc00`, false}, {`\ud800\u0041`, false},
		{`\ud83d\ude00`, true}, {`\\ud800`, true}, {`\u4f60\u597d`, true},
	} {
		wire := `{"type":"text","generation":1,"text":"` + tc.value + `"}`
		_, err := DecodeControl([]byte(wire))
		if (err == nil) != tc.valid {
			t.Errorf("%s: %v", wire, err)
		}
	}
}

func TestPCMRejectsMalformedFrames(t *testing.T) {
	valid, _ := EncodePCM(PCMHeader{Flags: 3, Generation: 1}, make([]byte, 640), true)
	for _, mutate := range []func([]byte) []byte{
		func(b []byte) []byte { return b[:15] },
		func(b []byte) []byte { b[3] = '1'; return b },
		func(b []byte) []byte { b[7] = 1; return b },
		func(b []byte) []byte { b[5] = 8; return b },
		func(b []byte) []byte { b[5] = 2; return b },
		func(b []byte) []byte { b[11] = 0; return b },
		func(b []byte) []byte { b[15] = 1; return b },
		func(b []byte) []byte { return b[:len(b)-2] },
	} {
		if _, _, err := ParsePCMFrame(mutate(append([]byte(nil), valid...)), true); err == nil {
			t.Fatal("accepted malformed PCM")
		}
	}
	for _, tc := range []struct {
		flags uint16
		size  int
		valid bool
	}{
		{3, 2, true}, {3, 960, true}, {3, 962, false}, {1, 2, false}, {1, 960, true}, {7, 960, false}, {3, 0, false}, {3, 3, false},
	} {
		_, err := EncodePCM(PCMHeader{Flags: tc.flags, Generation: 1}, make([]byte, tc.size), false)
		if (err == nil) != tc.valid {
			t.Errorf("%+v: %v", tc, err)
		}
	}
}

func FuzzDecodeV2(f *testing.F) {
	f.Add([]byte("{\"type\":\"ready\"}"))
	f.Add([]byte("BPV2"))
	f.Fuzz(func(t *testing.T, data []byte) {
		_, _ = DecodeControl(data)
		_, _, _ = ParsePCMFrame(data, true)
		_, _, _ = ParsePCMFrame(data, false)
	})
}
