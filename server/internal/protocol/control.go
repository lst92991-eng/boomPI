package protocol

import (
	"bytes"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"strconv"
	"strings"
	"unicode/utf8"
)

const (
	Version                = 1
	MaxControlMessageBytes = 64 * 1024
)

type ControlEnvelope struct {
	Version   uint8           `json:"version"`
	Type      string          `json:"type"`
	MessageID string          `json:"message_id"`
	DeviceID  string          `json:"device_id"`
	SessionID uint32          `json:"session_id"`
	TurnID    uint32          `json:"turn_id"`
	StreamID  uint32          `json:"stream_id"`
	Epoch     uint32          `json:"epoch"`
	Payload   json.RawMessage `json:"payload"`
}

func DecodeControl(data []byte) (ControlEnvelope, error) {
	if len(data) == 0 || len(data) > MaxControlMessageBytes {
		return ControlEnvelope{}, errors.New("control message length is outside the allowed range")
	}
	if !utf8.Valid(data) {
		return ControlEnvelope{}, errors.New("control message is not valid UTF-8")
	}
	if err := rejectDuplicateJSONKeys(data); err != nil {
		return ControlEnvelope{}, err
	}
	var fields map[string]json.RawMessage
	if err := json.Unmarshal(data, &fields); err != nil {
		return ControlEnvelope{}, fmt.Errorf("decode control fields: %w", err)
	}
	for _, required := range []string{"version", "type", "message_id", "device_id", "session_id", "turn_id", "stream_id", "epoch", "payload"} {
		if _, exists := fields[required]; !exists {
			return ControlEnvelope{}, fmt.Errorf("control envelope is missing field %q", required)
		}
	}
	numericFields := make(map[string]uint32, 5)
	for _, name := range []string{"version", "session_id", "turn_id", "stream_id", "epoch"} {
		value, err := decodeJSONUint32(fields[name])
		if err != nil {
			return ControlEnvelope{}, fmt.Errorf("control field %q: %w", name, err)
		}
		numericFields[name] = value
	}
	decoder := json.NewDecoder(bytes.NewReader(data))
	decoder.DisallowUnknownFields()
	var envelope ControlEnvelope
	if err := decoder.Decode(&envelope); err != nil {
		return ControlEnvelope{}, fmt.Errorf("decode control envelope: %w", err)
	}
	if err := ensureJSONEOF(decoder); err != nil {
		return ControlEnvelope{}, err
	}
	if numericFields["version"] != uint32(envelope.Version) ||
		numericFields["session_id"] != envelope.SessionID ||
		numericFields["turn_id"] != envelope.TurnID ||
		numericFields["stream_id"] != envelope.StreamID ||
		numericFields["epoch"] != envelope.Epoch {
		return ControlEnvelope{}, errors.New("decoded control numeric fields are inconsistent")
	}
	if err := envelope.Validate(); err != nil {
		return ControlEnvelope{}, err
	}
	return envelope, nil
}

func EncodeControl(envelope ControlEnvelope) ([]byte, error) {
	if err := envelope.Validate(); err != nil {
		return nil, err
	}
	encoded, err := json.Marshal(envelope)
	if err != nil {
		return nil, fmt.Errorf("encode control envelope: %w", err)
	}
	if len(encoded) > MaxControlMessageBytes {
		return nil, errors.New("encoded control message exceeds the maximum size")
	}
	return encoded, nil
}

func (e ControlEnvelope) Validate() error {
	if e.Version != Version {
		return fmt.Errorf("unsupported protocol version %d", e.Version)
	}
	if !validASCIIField(e.Type, 64) {
		return errors.New("control type must contain 1..64 ASCII bytes")
	}
	if strings.TrimSpace(e.MessageID) == "" || len(e.MessageID) > 64 {
		return errors.New("message_id must contain 1..64 bytes")
	}
	if _, err := ParseDeviceUUID(e.DeviceID); err != nil {
		return fmt.Errorf("device_id: %w", err)
	}
	payload := bytes.TrimSpace(e.Payload)
	if len(payload) < 2 || payload[0] != '{' || payload[len(payload)-1] != '}' || !json.Valid(payload) {
		return errors.New("payload must be a valid JSON object")
	}
	if err := rejectDuplicateJSONKeys(payload); err != nil {
		return fmt.Errorf("payload: %w", err)
	}
	return nil
}

func decodeJSONUint32(raw json.RawMessage) (uint32, error) {
	value := strings.TrimSpace(string(raw))
	if value == "" {
		return 0, errors.New("must be a JSON unsigned integer")
	}
	for index := 0; index < len(value); index++ {
		if value[index] < '0' || value[index] > '9' {
			return 0, errors.New("must be a JSON unsigned integer")
		}
	}
	parsed, err := strconv.ParseUint(value, 10, 32)
	if err != nil {
		return 0, errors.New("must be within the uint32 range")
	}
	return uint32(parsed), nil
}

func validASCIIField(value string, maxBytes int) bool {
	if value == "" || len(value) > maxBytes {
		return false
	}
	for index := 0; index < len(value); index++ {
		if value[index] < 0x21 || value[index] > 0x7e {
			return false
		}
	}
	return true
}

func rejectDuplicateJSONKeys(data []byte) error {
	decoder := json.NewDecoder(bytes.NewReader(data))
	decoder.UseNumber()
	if err := walkJSONValue(decoder); err != nil {
		return fmt.Errorf("validate JSON object keys: %w", err)
	}
	return ensureJSONEOF(decoder)
}

func walkJSONValue(decoder *json.Decoder) error {
	token, err := decoder.Token()
	if err != nil {
		return err
	}
	delimiter, isDelimiter := token.(json.Delim)
	if !isDelimiter {
		return nil
	}
	switch delimiter {
	case '{':
		seen := make(map[string]struct{})
		for decoder.More() {
			keyToken, err := decoder.Token()
			if err != nil {
				return err
			}
			key, ok := keyToken.(string)
			if !ok {
				return errors.New("object key is not a string")
			}
			if _, duplicate := seen[key]; duplicate {
				return fmt.Errorf("duplicate object key %q", key)
			}
			seen[key] = struct{}{}
			if err := walkJSONValue(decoder); err != nil {
				return err
			}
		}
		closing, err := decoder.Token()
		if err != nil {
			return err
		}
		if closing != json.Delim('}') {
			return errors.New("object is not terminated")
		}
	case '[':
		for decoder.More() {
			if err := walkJSONValue(decoder); err != nil {
				return err
			}
		}
		closing, err := decoder.Token()
		if err != nil {
			return err
		}
		if closing != json.Delim(']') {
			return errors.New("array is not terminated")
		}
	default:
		return errors.New("unexpected JSON delimiter")
	}
	return nil
}

func ensureJSONEOF(decoder *json.Decoder) error {
	var extra any
	if err := decoder.Decode(&extra); err == io.EOF {
		return nil
	} else if err != nil {
		return fmt.Errorf("decode trailing JSON: %w", err)
	}
	return errors.New("control message contains multiple JSON values")
}
