#!/usr/bin/env python3
"""Independently validate the shared v4 wire examples; no external dependencies."""

import json
import struct
import sys
import uuid
from pathlib import Path

HEADER = struct.Struct(">4sII")
def decode_control(wire):
    if not 0 < len(wire.encode("utf-8")) <= 8192 or "\0" in wire:
        raise ValueError("invalid control encoding/length")
    if wire == "READY 4 16000":
        return dict(type="ready", version=4, sample_rate=16000)
    if wire.startswith("HELLO 4 16000 "):
        identity, sep, token = wire[len("HELLO 4 16000 "):].partition(" ")
        if not sep or str(uuid.UUID(identity)) != identity or not 0 < len(token) <= 256:
            raise ValueError("invalid hello")
        if any(not 32 < ord(c) < 127 for c in token):
            raise ValueError("invalid token")
        return dict(type="hello", version=4, sample_rate=16000, device_id=identity, token=token)
    command, sep, rest = wire.partition(" ")
    if not sep:
        raise ValueError("missing generation")
    number, sep, body = rest.partition(" ")
    if not number or number[0] == '0' or any(c not in "0123456789" for c in number):
        raise ValueError("invalid generation")
    generation = int(number)
    if not 0 < generation < 2**32:
        raise ValueError("generation overflow")
    value = dict(type=command.lower(), generation=generation)
    if command in ("END", "DONE"):
        if sep:
            raise ValueError("extra field")
    elif command in ("START", "CANCEL"):
        if body not in ("0", "1"):
            raise ValueError("invalid flag")
        value["supersede" if command == "START" else "retract"] = body == "1"
    elif command == "TEXT":
        if not 0 < len(body.encode("utf-8")) <= 4096:
            raise ValueError("invalid text")
        value["text"] = body
    elif command == "ERROR":
        if not 0 < len(body) <= 64 or any(c not in "abcdefghijklmnopqrstuvwxyz0123456789_" for c in body):
            raise ValueError("invalid code")
        value["code"] = body
    else:
        raise ValueError("unknown control")
    return value


def validate_audio(item):
    header = bytes.fromhex(item["header_hex"])
    payload = bytes.fromhex(item["payload_hex"])
    assert len(header) == HEADER.size == 12
    assert bytes.fromhex(item["wire_hex"]) == header + payload
    magic, generation, sequence = HEADER.unpack(header)
    assert magic == b"BPV4" and generation != 0 and sequence < 2**32-1
    assert {"generation":generation,"sequence":sequence} == item["header"]
    assert len(payload)>0 and len(payload)%2==0
    assert len(payload)==640 if item["direction"]=="uplink" else len(payload)<=640


def main():
    root = Path(__file__).resolve().parents[1]
    document = json.loads((root / "protocol/fixtures/protocol-v4-golden.json").read_text(encoding="utf-8"))
    assert document["fixture_version"] == 4
    for item in document["control_frames"]:
        assert decode_control(item["wire_text"]) == item["expected"], item["name"]
    for item in document["audio_frames"]:
        validate_audio(item)
    for wire in document["invalid_control_frames"]:
        try:
            decode_control(wire)
        except (ValueError, TypeError, UnicodeError):
            continue
        raise ValueError("accepted invalid control: " + wire)
    print("validated v4 control, PCM, and malformed-control fixtures")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError, TypeError, AssertionError) as error:
        print("protocol fixture validation failed: {}".format(error), file=sys.stderr)
        sys.exit(1)
