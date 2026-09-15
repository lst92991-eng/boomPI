package protocol

import (
	"encoding/binary"
	"errors"
)

const (
	PCMHeaderSize      = 12
	UplinkFrameBytes   = 640
	DownlinkFrameBytes = 640
)

type PCMHeader struct {
	Generation uint32
	Sequence   uint32
}

func (header PCMHeader) validate(payloadBytes int, uplink bool) error {
	if header.Generation == 0 || header.Sequence == ^uint32(0) || payloadBytes == 0 || payloadBytes%2 != 0 {
		return errors.New("invalid PCM generation, sequence or sample alignment")
	}
	if uplink && payloadBytes != UplinkFrameBytes {
		return errors.New("uplink frame must contain 20 ms PCM")
	}
	if !uplink && payloadBytes > DownlinkFrameBytes {
		return errors.New("downlink frame exceeds 20 ms PCM")
	}
	return nil
}

func ParsePCMFrame(frame []byte, uplink bool) (PCMHeader, []byte, error) {
	var header PCMHeader
	if len(frame) < PCMHeaderSize || string(frame[:4]) != "BPV3" {
		return header, nil, errors.New("invalid BPV3 header")
	}
	header = PCMHeader{Generation: binary.BigEndian.Uint32(frame[4:8]), Sequence: binary.BigEndian.Uint32(frame[8:12])}
	if err := header.validate(len(frame)-PCMHeaderSize, uplink); err != nil {
		return header, nil, err
	}
	return header, frame[PCMHeaderSize:], nil
}

func EncodePCM(header PCMHeader, pcm []byte, uplink bool) ([]byte, error) {
	if err := header.validate(len(pcm), uplink); err != nil {
		return nil, err
	}
	frame := make([]byte, PCMHeaderSize+len(pcm))
	copy(frame, "BPV3")
	binary.BigEndian.PutUint32(frame[4:8], header.Generation)
	binary.BigEndian.PutUint32(frame[8:12], header.Sequence)
	copy(frame[PCMHeaderSize:], pcm)
	return frame, nil
}
