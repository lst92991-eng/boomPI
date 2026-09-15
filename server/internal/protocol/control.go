package protocol

import (
	"encoding/hex"
	"errors"
	"strconv"
	"strings"
	"unicode/utf8"
)

const MaxControlMessageBytes = 8192
const MaxTextBytes = 4096

type Control struct {
	Type       string `json:"type"`
	Version    int    `json:"version,omitempty"`
	Supersede  *bool  `json:"supersede,omitempty"`
	SampleRate int    `json:"sample_rate,omitempty"`
	DeviceID   string `json:"device_id,omitempty"`
	Token      string `json:"token,omitempty"`
	Generation uint32 `json:"generation,omitempty"`
	Text       string `json:"text,omitempty"`
	Code       string `json:"code,omitempty"`
	Retract    *bool  `json:"retract,omitempty"`
}

func generation(text string) (uint32, error) {
	if text == "" || text[0] == '0' {
		return 0, errors.New("invalid generation")
	}
	for _, c := range text {
		if c < '0' || c > '9' {
			return 0, errors.New("invalid generation")
		}
	}
	value, err := strconv.ParseUint(text, 10, 32)
	return uint32(value), err
}
func DecodeControl(data []byte) (Control, error) {
	var result Control
	invalid := errors.New("invalid control fields, version, length or UTF-8")
	if len(data) == 0 || len(data) > MaxControlMessageBytes || !utf8.Valid(data) || strings.IndexByte(string(data), 0) >= 0 {
		return result, invalid
	}
	text := string(data)
	if text == "READY 4 16000" {
		return Control{Type: "ready", Version: 4, SampleRate: 16000}, nil
	}
	if strings.HasPrefix(text, "HELLO 4 16000 ") {
		id, token, found := strings.Cut(strings.TrimPrefix(text, "HELLO 4 16000 "), " ")
		if !found || !ValidDeviceID(id) || len(token) == 0 || len(token) > 256 {
			return result, invalid
		}
		for _, c := range token {
			if c <= 32 || c > 126 {
				return result, invalid
			}
		}
		return Control{Type: "hello", Version: 4, SampleRate: 16000, DeviceID: id, Token: token}, nil
	}
	command, rest, found := strings.Cut(text, " ")
	if !found {
		return result, invalid
	}
	number, body, hasBody := strings.Cut(rest, " ")
	var err error
	result.Generation, err = generation(number)
	if err != nil {
		return result, invalid
	}
	result.Type = strings.ToLower(command)
	switch command {
	case "END", "DONE":
		if hasBody {
			return result, invalid
		}
	case "START", "CANCEL":
		if !hasBody || (body != "0" && body != "1") {
			return result, invalid
		}
		value := body == "1"
		if command == "START" {
			result.Supersede = &value
		} else {
			result.Retract = &value
		}
	case "TEXT":
		if !hasBody || len(body) == 0 || len(body) > MaxTextBytes {
			return result, invalid
		}
		result.Text = body
	case "ERROR":
		if !hasBody || len(body) == 0 || len(body) > 64 || strings.Trim(body, "abcdefghijklmnopqrstuvwxyz0123456789_") != "" {
			return result, invalid
		}
		result.Code = body
	default:
		return result, invalid
	}
	return result, nil
}
func EncodeControl(c Control) ([]byte, error) {
	var text string
	id := strconv.FormatUint(uint64(c.Generation), 10)
	switch c.Type {
	case "hello":
		text = "HELLO " + strconv.Itoa(c.Version) + " " + strconv.Itoa(c.SampleRate) + " " + c.DeviceID + " " + c.Token
	case "ready":
		text = "READY " + strconv.Itoa(c.Version) + " " + strconv.Itoa(c.SampleRate)
	case "start", "cancel":
		flag := c.Supersede
		if c.Type == "cancel" {
			flag = c.Retract
		}
		if flag == nil {
			return nil, errors.New("control flag is required")
		}
		value := "0"
		if *flag {
			value = "1"
		}
		text = strings.ToUpper(c.Type) + " " + id + " " + value
	case "end", "done":
		text = strings.ToUpper(c.Type) + " " + id
	case "text":
		text = "TEXT " + id + " " + c.Text
	case "error":
		text = "ERROR " + id + " " + c.Code
	default:
		return nil, errors.New("unknown control type")
	}
	data := []byte(text)
	_, err := DecodeControl(data)
	return data, err
}
func ValidDeviceID(id string) bool {
	if len(id) != 36 || id[8] != '-' || id[13] != '-' || id[18] != '-' || id[23] != '-' || strings.ToLower(id) != id {
		return false
	}
	_, err := hex.DecodeString(strings.ReplaceAll(id, "-", ""))
	return err == nil
}
