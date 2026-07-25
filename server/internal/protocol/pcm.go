package protocol

import (
	"bytes"
	"encoding/binary"
	"encoding/hex"
	"errors"
	"fmt"
)

const (
	PCMHeaderSize      = 64
	MaxPCMPayloadBytes = 64 * 1024
	AudioFormatPCM16LE = 1
	AudioKindUplink    = 1
	AudioKindDownlink  = 2
)

var pcmMagic = [4]byte{'B', 'P', 'V', '1'}

const allowedPCMFlags uint16 = 0x0007

type PCMHeader struct {
	Version      uint8
	Kind         uint8
	Flags        uint16
	AudioFormat  uint8
	Channels     uint8
	SampleRateHz uint32
	PayloadLen   uint32
	Sequence     uint32
	TimestampUS  uint64
	Epoch        uint32
	DeviceUUID   [16]byte
	SessionID    uint32
	TurnID       uint32
	StreamID     uint32
}

type PCMStreamContext struct {
	Kind             uint8
	SampleRateHz     uint32
	DeviceUUID       [16]byte
	Epoch            uint32
	SessionID        uint32
	TurnID           uint32
	StreamID         uint32
	ExpectedSequence uint32
}

func (h PCMHeader) Validate() error {
	if h.Version != Version {
		return fmt.Errorf("unsupported PCM protocol version %d", h.Version)
	}
	if h.Kind != AudioKindUplink && h.Kind != AudioKindDownlink {
		return errors.New("invalid PCM kind")
	}
	if h.AudioFormat != AudioFormatPCM16LE {
		return errors.New("unsupported PCM audio format")
	}
	if h.Flags & ^allowedPCMFlags != 0 {
		return errors.New("PCM flags contain reserved bits")
	}
	if h.Channels != 1 {
		return errors.New("PCM channels must be 1 in protocol v1")
	}
	if h.SampleRateHz < 8000 || h.SampleRateHz > 96000 {
		return errors.New("PCM sample rate must be between 8000 and 96000 Hz")
	}
	if h.PayloadLen == 0 || h.PayloadLen > MaxPCMPayloadBytes {
		return errors.New("PCM payload exceeds the maximum size")
	}
	if h.PayloadLen%uint32(h.Channels*2) != 0 {
		return errors.New("PCM payload is not aligned to S16_LE samples")
	}
	if h.Epoch == 0 || h.SessionID == 0 || h.TurnID == 0 || h.StreamID == 0 {
		return errors.New("active PCM identifiers must be nonzero")
	}
	if h.DeviceUUID == ([16]byte{}) {
		return errors.New("PCM device UUID must be nonzero")
	}
	return nil
}

func (c PCMStreamContext) ValidateHeader(header PCMHeader) error {
	if err := header.Validate(); err != nil {
		return err
	}
	if c.Kind != AudioKindUplink && c.Kind != AudioKindDownlink {
		return errors.New("PCM stream context kind is invalid")
	}
	if c.SampleRateHz < 8000 || c.SampleRateHz > 96000 {
		return errors.New("PCM stream context sample rate is invalid")
	}
	if c.DeviceUUID == ([16]byte{}) || c.Epoch == 0 || c.SessionID == 0 || c.TurnID == 0 || c.StreamID == 0 {
		return errors.New("PCM stream context identifiers must be nonzero")
	}
	if header.Kind != c.Kind {
		return errors.New("PCM kind does not match the active stream")
	}
	if header.SampleRateHz != c.SampleRateHz {
		return errors.New("PCM sample rate does not match the active stream")
	}
	if header.DeviceUUID != c.DeviceUUID {
		return errors.New("PCM device UUID does not match the active stream")
	}
	if header.Epoch != c.Epoch {
		return errors.New("PCM epoch does not match the active stream")
	}
	if header.SessionID != c.SessionID {
		return errors.New("PCM session ID does not match the active stream")
	}
	if header.TurnID != c.TurnID {
		return errors.New("PCM turn ID does not match the active stream")
	}
	if header.StreamID != c.StreamID {
		return errors.New("PCM stream ID does not match the active stream")
	}
	if header.Sequence != c.ExpectedSequence {
		return errors.New("PCM sequence does not match the active stream")
	}
	return nil
}

func ParseDeviceUUID(value string) ([16]byte, error) {
	var result [16]byte
	if len(value) != 36 || value[8] != '-' || value[13] != '-' || value[18] != '-' || value[23] != '-' {
		return result, errors.New("must be a canonical UUID")
	}
	compact := value[0:8] + value[9:13] + value[14:18] + value[19:23] + value[24:36]
	for _, current := range compact {
		if !((current >= '0' && current <= '9') || (current >= 'a' && current <= 'f')) {
			return result, errors.New("must use lowercase canonical UUID hexadecimal")
		}
	}
	decoded, err := hex.DecodeString(compact)
	if err != nil || len(decoded) != len(result) {
		return result, errors.New("must be a canonical UUID")
	}
	copy(result[:], decoded)
	if result == ([16]byte{}) {
		return result, errors.New("must not be the nil UUID")
	}
	return result, nil
}

func (h PCMHeader) MarshalBinary() ([]byte, error) {
	if err := h.Validate(); err != nil {
		return nil, err
	}
	buffer := make([]byte, PCMHeaderSize)
	copy(buffer[0:4], pcmMagic[:])
	buffer[4] = h.Version
	buffer[5] = h.Kind
	binary.BigEndian.PutUint16(buffer[6:8], h.Flags)
	binary.BigEndian.PutUint16(buffer[8:10], PCMHeaderSize)
	buffer[10] = h.AudioFormat
	buffer[11] = h.Channels
	binary.BigEndian.PutUint32(buffer[12:16], h.SampleRateHz)
	binary.BigEndian.PutUint32(buffer[16:20], h.PayloadLen)
	binary.BigEndian.PutUint32(buffer[20:24], h.Sequence)
	binary.BigEndian.PutUint64(buffer[24:32], h.TimestampUS)
	binary.BigEndian.PutUint32(buffer[32:36], h.Epoch)
	copy(buffer[36:52], h.DeviceUUID[:])
	binary.BigEndian.PutUint32(buffer[52:56], h.SessionID)
	binary.BigEndian.PutUint32(buffer[56:60], h.TurnID)
	binary.BigEndian.PutUint32(buffer[60:64], h.StreamID)
	return buffer, nil
}

func ParsePCMHeader(data []byte) (PCMHeader, error) {
	if len(data) < PCMHeaderSize {
		return PCMHeader{}, errors.New("PCM header is truncated")
	}
	if !bytes.Equal(data[0:4], pcmMagic[:]) {
		return PCMHeader{}, errors.New("PCM magic does not match BPV1")
	}
	if binary.BigEndian.Uint16(data[8:10]) != PCMHeaderSize {
		return PCMHeader{}, errors.New("PCM header length is not 64 bytes")
	}
	var header PCMHeader
	header.Version = data[4]
	header.Kind = data[5]
	header.Flags = binary.BigEndian.Uint16(data[6:8])
	header.AudioFormat = data[10]
	header.Channels = data[11]
	header.SampleRateHz = binary.BigEndian.Uint32(data[12:16])
	header.PayloadLen = binary.BigEndian.Uint32(data[16:20])
	header.Sequence = binary.BigEndian.Uint32(data[20:24])
	header.TimestampUS = binary.BigEndian.Uint64(data[24:32])
	header.Epoch = binary.BigEndian.Uint32(data[32:36])
	copy(header.DeviceUUID[:], data[36:52])
	header.SessionID = binary.BigEndian.Uint32(data[52:56])
	header.TurnID = binary.BigEndian.Uint32(data[56:60])
	header.StreamID = binary.BigEndian.Uint32(data[60:64])
	if err := header.Validate(); err != nil {
		return PCMHeader{}, err
	}
	return header, nil
}

func ParsePCMFrame(frame []byte) (PCMHeader, []byte, error) {
	header, err := ParsePCMHeader(frame)
	if err != nil {
		return PCMHeader{}, nil, err
	}
	expected := PCMHeaderSize + int(header.PayloadLen)
	if len(frame) != expected {
		return PCMHeader{}, nil, fmt.Errorf("PCM frame length is %d, expected %d", len(frame), expected)
	}
	return header, frame[PCMHeaderSize:], nil
}
