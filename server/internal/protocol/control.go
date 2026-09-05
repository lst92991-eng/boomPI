package protocol

import (
	"bytes"
	"encoding/hex"
	"encoding/json"
	"errors"
	"io"
	"strconv"
	"strings"
	"unicode/utf8"
)

const MaxControlMessageBytes = 8192
const MaxTextBytes = 4096

// Control is the complete v2 JSON vocabulary. Each message has one exact shape.
type Control struct {
	Type       string `json:"type"`
	DeviceID   string `json:"device_id,omitempty"`
	Token      string `json:"token,omitempty"`
	Generation uint32 `json:"generation,omitempty"`
	Text       string `json:"text,omitempty"`
	Code       string `json:"code,omitempty"`
	Retract    *bool  `json:"retract,omitempty"`
}

func DecodeControl(data []byte) (Control, error) {
	var result Control
	if len(data) == 0 || len(data) > MaxControlMessageBytes || !utf8.Valid(data) || !validEscapedUnicode(data) {
		return result, errors.New("invalid JSON length or UTF-8")
	}
	decoder := json.NewDecoder(bytes.NewReader(data))
	token, err := decoder.Token()
	if err != nil || token != json.Delim('{') {
		return result, errors.New("control must be an object")
	}
	fields := make(map[string]json.RawMessage)
	for decoder.More() {
		token, err = decoder.Token()
		key, ok := token.(string)
		if err != nil || !ok || fields[key] != nil {
			return result, errors.New("duplicate or invalid JSON key")
		}
		var value json.RawMessage
		if err = decoder.Decode(&value); err != nil || bytes.Equal(value, []byte("null")) {
			return result, errors.New("invalid JSON value")
		}
		fields[key] = value
	}
	if _, err = decoder.Token(); err != nil {
		return result, err
	}
	if _, err = decoder.Token(); err != io.EOF {
		return result, errors.New("trailing JSON data")
	}
	if err = json.Unmarshal(data, &result); err != nil {
		return result, errors.New("invalid control field type")
	}
	want := []string{"type"}
	switch result.Type {
	case "hello":
		want = append(want, "device_id", "token")
		if !ValidDeviceID(result.DeviceID) || len(result.Token) == 0 || len(result.Token) > 256 {
			return result, errors.New("invalid hello identity")
		}
	case "ready":
	case "text":
		want = append(want, "generation", "text")
		if len(result.Text) == 0 || len(result.Text) > MaxTextBytes {
			return result, errors.New("invalid text delta length")
		}
	case "done":
		want = append(want, "generation")
	case "error":
		want = append(want, "generation", "code")
		if len(result.Code) == 0 || len(result.Code) > 64 || strings.Trim(result.Code, "abcdefghijklmnopqrstuvwxyz0123456789_") != "" {
			return result, errors.New("invalid error code")
		}
	case "stop":
		want = append(want, "generation", "retract")
	default:
		return result, errors.New("unknown control type")
	}
	if len(fields) != len(want) {
		return result, errors.New("unexpected control fields")
	}
	for _, key := range want {
		if fields[key] == nil {
			return result, errors.New("missing control field")
		}
	}
	if fields["generation"] != nil && result.Generation == 0 {
		return result, errors.New("generation must be nonzero")
	}
	return result, nil
}

// encoding/json replaces lone UTF-16 surrogates with U+FFFD. Reject them at
// the wire boundary so Go and the board agree on the actual text and identity.
func validEscapedUnicode(data []byte) bool {
	for i := 0; i < len(data); i++ {
		if data[i] != '\\' {
			continue
		}
		i++
		if i >= len(data) {
			return false
		}
		if data[i] != 'u' {
			continue
		}
		if i+4 >= len(data) {
			return false
		}
		code, err := strconv.ParseUint(string(data[i+1:i+5]), 16, 16)
		if err != nil {
			return false
		}
		if code >= 0xdc00 && code <= 0xdfff {
			return false
		}
		if code >= 0xd800 && code <= 0xdbff {
			if i+10 >= len(data) || data[i+5] != '\\' || data[i+6] != 'u' {
				return false
			}
			low, err := strconv.ParseUint(string(data[i+7:i+11]), 16, 16)
			if err != nil || low < 0xdc00 || low > 0xdfff {
				return false
			}
			i += 10
		} else {
			i += 4
		}
	}
	return true
}

func EncodeControl(control Control) ([]byte, error) {
	data, err := json.Marshal(control)
	if err == nil {
		_, err = DecodeControl(data)
	}
	return data, err
}

func ValidDeviceID(id string) bool {
	if len(id) != 36 || id[8] != '-' || id[13] != '-' || id[18] != '-' || id[23] != '-' || strings.ToLower(id) != id {
		return false
	}
	_, err := hex.DecodeString(strings.ReplaceAll(id, "-", ""))
	return err == nil
}
