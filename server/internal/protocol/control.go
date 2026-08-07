package protocol

import (
	"bytes"
	"encoding/json"
	"errors"
	"fmt"
	"io"
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
	if err := rejectDuplicateObjectKeys(data); err != nil {
		return ControlEnvelope{}, fmt.Errorf("validate control object keys: %w", err)
	}
	var fields map[string]json.RawMessage
	if err := json.Unmarshal(data, &fields); err != nil {
		return ControlEnvelope{}, fmt.Errorf("decode control fields: %w", err)
	}
	requiredFields := [...]string{"version", "type", "message_id", "device_id", "session_id", "turn_id", "stream_id", "epoch", "payload"}
	if len(fields) != len(requiredFields) {
		return ControlEnvelope{}, fmt.Errorf("control envelope must contain exactly %d fields", len(requiredFields))
	}
	for _, required := range requiredFields {
		if _, exists := fields[required]; !exists {
			return ControlEnvelope{}, fmt.Errorf("control envelope is missing field %q", required)
		}
	}
	for _, numeric := range []string{"version", "session_id", "turn_id", "stream_id", "epoch"} {
		if bytes.Equal(bytes.TrimSpace(fields[numeric]), []byte("null")) {
			return ControlEnvelope{}, fmt.Errorf("control field %q must be an unsigned integer", numeric)
		}
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
	if err := envelope.Validate(); err != nil {
		return ControlEnvelope{}, err
	}
	return envelope, nil
}

func rejectDuplicateObjectKeys(data []byte) error {
	decoder := json.NewDecoder(bytes.NewReader(data))
	if err := scanJSONValue(decoder); err != nil {
		return err
	}
	return ensureJSONEOF(decoder)
}

func scanJSONValue(decoder *json.Decoder) error {
	token, err := decoder.Token()
	if err != nil {
		return err
	}
	delimiter, composite := token.(json.Delim)
	if !composite {
		return nil
	}
	switch delimiter {
	case '{':
		keys := make(map[string]struct{})
		for decoder.More() {
			keyToken, keyErr := decoder.Token()
			if keyErr != nil {
				return keyErr
			}
			key, ok := keyToken.(string)
			if !ok {
				return errors.New("JSON object key is not a string")
			}
			if _, duplicate := keys[key]; duplicate {
				return fmt.Errorf("duplicate JSON object key %q", key)
			}
			keys[key] = struct{}{}
			if err := scanJSONValue(decoder); err != nil {
				return err
			}
		}
		_, err = decoder.Token()
	case '[':
		for decoder.More() {
			if err := scanJSONValue(decoder); err != nil {
				return err
			}
		}
		_, err = decoder.Token()
	default:
		return errors.New("unexpected JSON delimiter")
	}
	return err
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
	return nil
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

func ensureJSONEOF(decoder *json.Decoder) error {
	var extra any
	if err := decoder.Decode(&extra); err == io.EOF {
		return nil
	} else if err != nil {
		return fmt.Errorf("decode trailing JSON: %w", err)
	}
	return errors.New("control message contains multiple JSON values")
}
