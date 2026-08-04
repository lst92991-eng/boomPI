package protocol

import (
	"bytes"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"reflect"
	"runtime"
	"strings"
	"testing"
)

func TestControlEnvelopeRoundTrip(t *testing.T) {
	want := ControlEnvelope{
		Version: 1, Type: "turn.start", MessageID: "message-1", DeviceID: "00112233-4455-6677-8899-aabbccddeeff",
		SessionID: 2, TurnID: 3, StreamID: 4, Epoch: 5,
		Payload: json.RawMessage(`{"capabilities":["audio"]}`),
	}
	encoded, err := EncodeControl(want)
	if err != nil {
		t.Fatalf("EncodeControl() error = %v", err)
	}
	got, err := DecodeControl(encoded)
	if err != nil {
		t.Fatalf("DecodeControl() error = %v", err)
	}
	if !reflect.DeepEqual(got, want) {
		t.Fatalf("round trip = %#v", got)
	}
}

func TestControlEnvelopeRejectsInvalidNumericFields(t *testing.T) {
	for _, field := range []string{"version", "session_id", "turn_id", "stream_id", "epoch"} {
		for _, value := range []string{"null", `"1"`, "1.5", "-1", "4294967296", "true"} {
			t.Run(field+"_"+strings.NewReplacer(`"`, "quote", ".", "dot", "-", "negative").Replace(value), func(t *testing.T) {
				data := controlJSONWithNumericField(field, value)
				if _, err := DecodeControl(data); err == nil {
					t.Fatalf("DecodeControl() accepted %s=%s", field, value)
				}
			})
		}
	}
}

func TestControlEnvelopeAcceptsMaximumUint32IDs(t *testing.T) {
	data := []byte(`{"version":1,"type":"turn.start","message_id":"1","device_id":"00112233-4455-6677-8899-aabbccddeeff","session_id":4294967295,"turn_id":4294967295,"stream_id":4294967295,"epoch":4294967295,"payload":{}}`)
	got, err := DecodeControl(data)
	if err != nil {
		t.Fatalf("DecodeControl() error = %v", err)
	}
	if got.SessionID != ^uint32(0) || got.TurnID != ^uint32(0) || got.StreamID != ^uint32(0) || got.Epoch != ^uint32(0) {
		t.Fatalf("decoded IDs = %#v", got)
	}
}

func TestControlEnvelopeRejectsUnknownField(t *testing.T) {
	data := []byte(`{"version":1,"type":"hello","message_id":"1","device_id":"00112233-4455-6677-8899-aabbccddeeff","session_id":0,"turn_id":0,"stream_id":0,"epoch":0,"payload":{},"extra":true}`)
	if _, err := DecodeControl(data); err == nil {
		t.Fatal("DecodeControl() unexpectedly accepted an unknown field")
	}
}

func TestControlEnvelopeRejectsDuplicateKeysAndNonObjectPayload(t *testing.T) {
	duplicate := []byte(`{"version":1,"type":"hello","type":"turn.start","message_id":"1","device_id":"00112233-4455-6677-8899-aabbccddeeff","session_id":0,"turn_id":0,"stream_id":0,"epoch":1,"payload":{}}`)
	if _, err := DecodeControl(duplicate); err == nil {
		t.Fatal("DecodeControl() unexpectedly accepted a duplicate key")
	}
	nonObject := ControlEnvelope{Version: 1, Type: "hello", MessageID: "1", DeviceID: "00112233-4455-6677-8899-aabbccddeeff", Payload: json.RawMessage(`[]`)}
	if _, err := EncodeControl(nonObject); err == nil {
		t.Fatal("EncodeControl() unexpectedly accepted an array payload")
	}
	duplicatePayload := ControlEnvelope{
		Version: 1, Type: "turn.start", MessageID: "1", DeviceID: "00112233-4455-6677-8899-aabbccddeeff",
		SessionID: 1, TurnID: 1, StreamID: 1, Epoch: 1,
		Payload: json.RawMessage(`{"nested":{"value":1,"value":2}}`),
	}
	if _, err := EncodeControl(duplicatePayload); err == nil {
		t.Fatal("EncodeControl() unexpectedly accepted duplicate payload keys")
	}
}

func TestControlEnvelopeRejectsDeeplyNestedPayload(t *testing.T) {
	// 深度构造的嵌套不应耗尽解析栈：显式深度上限必须早于任何内部限制拒绝。
	nested := "0"
	for index := 0; index < 64; index++ {
		nested = "[" + nested + "]"
	}
	data := []byte(fmt.Sprintf(
		`{"version":1,"type":"hello","message_id":"1","device_id":"00112233-4455-6677-8899-aabbccddeeff","session_id":0,"turn_id":0,"stream_id":0,"epoch":1,"payload":{"deep":%s}}`,
		nested,
	))
	// 断言错误来自深度上限，防止未来其他校验碰巧报错造成假绿。
	if _, err := DecodeControl(data); err == nil || !strings.Contains(err.Error(), "depth limit") {
		t.Fatalf("DecodeControl() error = %v, want depth limit rejection", err)
	}
}

func controlJSONWithNumericField(field, value string) []byte {
	values := map[string]string{
		"version": "1", "session_id": "0", "turn_id": "0", "stream_id": "0", "epoch": "1",
	}
	values[field] = value
	return []byte(fmt.Sprintf(
		`{"version":%s,"type":"hello","message_id":"1","device_id":"00112233-4455-6677-8899-aabbccddeeff","session_id":%s,"turn_id":%s,"stream_id":%s,"epoch":%s,"payload":{}}`,
		values["version"], values["session_id"], values["turn_id"], values["stream_id"], values["epoch"],
	))
}

func TestPCMHeaderRoundTrip(t *testing.T) {
	device, err := ParseDeviceUUID("00112233-4455-6677-8899-aabbccddeeff")
	if err != nil {
		t.Fatalf("ParseDeviceUUID() error = %v", err)
	}
	want := PCMHeader{
		Version: 1, Kind: AudioKindUplink, Flags: 2, AudioFormat: AudioFormatPCM16LE,
		Channels: 1, SampleRateHz: 16000, PayloadLen: 640, Sequence: 7,
		TimestampUS: 123456789, Epoch: 8, DeviceUUID: device, SessionID: 9, TurnID: 10, StreamID: 11,
	}
	encoded, err := want.MarshalBinary()
	if err != nil {
		t.Fatalf("MarshalBinary() error = %v", err)
	}
	if len(encoded) != PCMHeaderSize {
		t.Fatalf("header length = %d", len(encoded))
	}
	got, err := ParsePCMHeader(encoded)
	if err != nil {
		t.Fatalf("ParsePCMHeader() error = %v", err)
	}
	if got != want {
		t.Fatalf("round trip = %#v, want %#v", got, want)
	}
}

func TestPCMFrameRejectsLengthMismatch(t *testing.T) {
	device, err := ParseDeviceUUID("00112233-4455-6677-8899-aabbccddeeff")
	if err != nil {
		t.Fatalf("ParseDeviceUUID() error = %v", err)
	}
	header := PCMHeader{Version: 1, Kind: AudioKindDownlink, AudioFormat: AudioFormatPCM16LE, Channels: 1, SampleRateHz: 24000, PayloadLen: 10, Epoch: 1, DeviceUUID: device, SessionID: 1, TurnID: 1, StreamID: 1}
	encoded, err := header.MarshalBinary()
	if err != nil {
		t.Fatalf("MarshalBinary() error = %v", err)
	}
	if _, _, err := ParsePCMFrame(encoded); err == nil {
		t.Fatal("ParsePCMFrame() unexpectedly accepted a missing payload")
	}
}

func TestPCMStreamContextRejectsStreamMismatches(t *testing.T) {
	device, err := ParseDeviceUUID("00112233-4455-6677-8899-aabbccddeeff")
	if err != nil {
		t.Fatalf("ParseDeviceUUID() error = %v", err)
	}
	header := PCMHeader{
		Version: 1, Kind: AudioKindUplink, AudioFormat: AudioFormatPCM16LE, Channels: 1,
		SampleRateHz: 16000, PayloadLen: 640, Sequence: 42, Epoch: 7, DeviceUUID: device,
		SessionID: 8, TurnID: 9, StreamID: 10,
	}
	stream := PCMStreamContext{
		Kind: AudioKindUplink, SampleRateHz: 16000, DeviceUUID: device, Epoch: 7,
		SessionID: 8, TurnID: 9, StreamID: 10, ExpectedSequence: 42,
	}
	if err := stream.ValidateHeader(header); err != nil {
		t.Fatalf("ValidateHeader() valid header error = %v", err)
	}
	tests := []struct {
		name   string
		mutate func(*PCMHeader)
	}{
		{name: "epoch", mutate: func(value *PCMHeader) { value.Epoch++ }},
		{name: "sample_rate", mutate: func(value *PCMHeader) { value.SampleRateHz = 24000 }},
		{name: "sequence", mutate: func(value *PCMHeader) { value.Sequence++ }},
	}
	for _, test := range tests {
		t.Run(test.name, func(t *testing.T) {
			candidate := header
			test.mutate(&candidate)
			if err := stream.ValidateHeader(candidate); err == nil {
				t.Fatal("ValidateHeader() unexpectedly accepted a stream mismatch")
			}
		})
	}
}

func TestGoldenFixture(t *testing.T) {
	_, sourceFile, _, ok := runtime.Caller(0)
	if !ok {
		t.Fatal("runtime.Caller() could not locate the test source")
	}
	repositoryRoot := filepath.Clean(filepath.Join(filepath.Dir(sourceFile), "..", "..", ".."))
	data, err := os.ReadFile(filepath.Join(repositoryRoot, "protocol", "fixtures", "protocol-v1-golden.json"))
	if err != nil {
		t.Fatalf("ReadFile() error = %v", err)
	}
	var fixture struct {
		ControlFrames []struct {
			WireText string `json:"wire_text"`
		} `json:"control_frames"`
		AudioFrames []struct {
			HeaderHex string `json:"header_hex"`
			WireHex   string `json:"wire_hex"`
		} `json:"audio_frames"`
	}
	if err := json.Unmarshal(data, &fixture); err != nil {
		t.Fatalf("fixture JSON error = %v", err)
	}
	if len(fixture.ControlFrames) != 1 || len(fixture.AudioFrames) != 1 {
		t.Fatalf("unexpected fixture cardinality: %#v", fixture)
	}
	if _, err := DecodeControl([]byte(fixture.ControlFrames[0].WireText)); err != nil {
		t.Fatalf("DecodeControl(golden) error = %v", err)
	}
	headerBytes, err := hex.DecodeString(fixture.AudioFrames[0].HeaderHex)
	if err != nil {
		t.Fatalf("DecodeString(header) error = %v", err)
	}
	header, err := ParsePCMHeader(headerBytes)
	if err != nil {
		t.Fatalf("ParsePCMHeader(golden) error = %v", err)
	}
	encoded, err := header.MarshalBinary()
	if err != nil {
		t.Fatalf("MarshalBinary(golden) error = %v", err)
	}
	if !bytes.Equal(encoded, headerBytes) {
		t.Fatalf("encoded golden header does not match fixture")
	}
	wireBytes, err := hex.DecodeString(fixture.AudioFrames[0].WireHex)
	if err != nil {
		t.Fatalf("DecodeString(wire) error = %v", err)
	}
	if _, payload, err := ParsePCMFrame(wireBytes); err != nil || len(payload) != 8 {
		t.Fatalf("ParsePCMFrame(golden) payload=%d error=%v", len(payload), err)
	}
}
