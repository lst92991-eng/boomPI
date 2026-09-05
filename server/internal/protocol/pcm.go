package protocol

import (
	"encoding/binary"
	"errors"
)

const (
	PCMHeaderSize             = 16
	UplinkFrameBytes          = 640
	DownlinkFrameBytes        = 960
	PCMFlagStart       uint16 = 1
	PCMFlagEnd         uint16 = 2
	PCMFlagSupersede   uint16 = 4
)

type PCMHeader struct {
	Flags      uint16
	Generation uint32
	Sequence   uint32
}

func (header PCMHeader) validate(payloadBytes int, uplink bool) error {
	if header.Generation == 0 || header.Flags & ^uint16(7) != 0 || payloadBytes == 0 || payloadBytes%2 != 0 {
		return errors.New("invalid PCM header or sample alignment")
	}
	start := header.Flags&PCMFlagStart != 0
	if start != (header.Sequence == 0) {
		return errors.New("START must occur exactly at sequence zero")
	}
	if header.Flags&PCMFlagSupersede != 0 && (!uplink || !start) {
		return errors.New("SUPERSEDE requires uplink START")
	}
	if uplink && payloadBytes != UplinkFrameBytes {
		return errors.New("uplink frame must contain 20 ms of PCM")
	}
	if !uplink && (payloadBytes > DownlinkFrameBytes || (header.Flags&PCMFlagEnd == 0 && payloadBytes != DownlinkFrameBytes)) {
		return errors.New("downlink frame length is invalid")
	}
	return nil
}

func ParsePCMFrame(frame []byte, uplink bool) (PCMHeader, []byte, error) {
	var header PCMHeader
	if len(frame) < PCMHeaderSize || string(frame[:4]) != "BPV2" || binary.BigEndian.Uint16(frame[6:8]) != 0 {
		return header, nil, errors.New("invalid BPV2 header")
	}
	header = PCMHeader{Flags: binary.BigEndian.Uint16(frame[4:6]), Generation: binary.BigEndian.Uint32(frame[8:12]), Sequence: binary.BigEndian.Uint32(frame[12:16])}
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
	copy(frame, "BPV2")
	binary.BigEndian.PutUint16(frame[4:6], header.Flags)
	binary.BigEndian.PutUint32(frame[8:12], header.Generation)
	binary.BigEndian.PutUint32(frame[12:16], header.Sequence)
	copy(frame[PCMHeaderSize:], pcm)
	return frame, nil
}
