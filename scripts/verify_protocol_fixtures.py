#!/usr/bin/env python3
"""Independently validate the shared v2 wire examples; no external dependencies."""

import json
import struct
import sys
import uuid
from pathlib import Path

HEADER = struct.Struct(">4sHHII")
SHAPES = {
    "hello": {"type", "device_id", "token"},
    "ready": {"type"},
    "text": {"type", "generation", "text"},
    "done": {"type", "generation"},
    "error": {"type", "generation", "code"},
    "stop": {"type", "generation", "retract"},
}


def no_duplicate_object_pairs(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError("duplicate JSON key")
        result[key] = value
    return result


def decode_control(wire):
    if not 0 < len(wire.encode("utf-8")) <= 8192:
        raise ValueError("invalid JSON length")
    value = json.loads(wire, object_pairs_hook=no_duplicate_object_pairs)
    if not isinstance(value, dict) or set(value) != SHAPES.get(value.get("type")):
        raise ValueError("wrong control shape")
    if "generation" in value:
        generation = value["generation"]
        if type(generation) is not int or not 1 <= generation < 2**32:
            raise ValueError("invalid generation")
    if value["type"] == "hello":
        identity = value["device_id"]
        if not isinstance(identity, str) or str(uuid.UUID(identity)) != identity:
            raise ValueError("invalid canonical UUID")
        if not isinstance(value["token"], str) or not 0 < len(value["token"].encode()) <= 256:
            raise ValueError("invalid token")
    if value["type"] == "stop" and type(value["retract"]) is not bool:
        raise ValueError("retract must be a boolean")
    if value["type"] == "text":
        if not isinstance(value["text"], str) or not 0 < len(value["text"].encode()) <= 4096:
            raise ValueError("invalid text")
    if value["type"] == "error":
        code = value["code"]
        if not isinstance(code, str) or not 0 < len(code) <= 64 or any(c not in "abcdefghijklmnopqrstuvwxyz0123456789_" for c in code):
            raise ValueError("invalid error code")
    return value


def validate_audio(item):
    header = bytes.fromhex(item["header_hex"])
    payload = bytes.fromhex(item["payload_hex"])
    assert len(header) == HEADER.size == 16
    assert bytes.fromhex(item["wire_hex"]) == header + payload
    magic, flags, reserved, generation, sequence = HEADER.unpack(header)
    assert magic == b"BPV2" and reserved == 0 and flags & ~7 == 0
    assert generation != 0 and bool(flags & 1) == (sequence == 0)
    assert {"flags": flags, "generation": generation, "sequence": sequence} == item["header"]
    assert len(payload) > 0 and len(payload) % 2 == 0
    if item["direction"] == "uplink":
        assert len(payload) == 640
        assert not flags & 4 or flags & 1
    else:
        assert item["direction"] == "downlink" and not flags & 4
        assert len(payload) <= 960 and (flags & 2 or len(payload) == 960)


def main():
    root = Path(__file__).resolve().parents[1]
    document = json.loads((root / "protocol/fixtures/protocol-v2-golden.json").read_text(encoding="utf-8"))
    assert document["fixture_version"] == 2
    for item in document["control_frames"]:
        assert decode_control(item["wire_text"]) == item["expected"], item["name"]
    for item in document["audio_frames"]:
        validate_audio(item)
    for wire in document["invalid_control_frames"]:
        try:
            decode_control(wire)
        except (ValueError, TypeError, UnicodeError):
            continue
        raise ValueError("accepted invalid JSON: " + wire)
    print("validated v2 control, PCM, and malformed-control fixtures")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError, TypeError, AssertionError) as error:
        print("protocol fixture validation failed: {}".format(error), file=sys.stderr)
        sys.exit(1)
