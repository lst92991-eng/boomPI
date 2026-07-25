#!/usr/bin/env python3
"""Validate boomPI protocol golden fixtures using only the standard library."""

import json
import struct
import sys
import uuid
from pathlib import Path


HEADER = struct.Struct(">4sBBHHBBIIIQI16sIII")
REQUIRED_ENVELOPE_FIELDS = {
    "version",
    "type",
    "message_id",
    "device_id",
    "session_id",
    "turn_id",
    "stream_id",
    "epoch",
    "payload",
}
UINT32_MAX = (1 << 32) - 1


def fail(message):
    raise ValueError(message)


def no_duplicate_object_pairs(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            fail("duplicate JSON key: {!r}".format(key))
        result[key] = value
    return result


def validate_u32(value, field_name, allow_zero=True):
    if isinstance(value, bool) or not isinstance(value, int):
        fail("{} must be an integer".format(field_name))
    minimum = 0 if allow_zero else 1
    if value < minimum or value > UINT32_MAX:
        fail("{} is outside the allowed u32 range".format(field_name))


def validate_control_fixture(item):
    wire = item.get("wire_text")
    if not isinstance(wire, str) or not wire:
        fail("control fixture {!r} has no wire_text".format(item.get("name")))
    if len(wire.encode("utf-8")) > 65536:
        fail("control fixture {!r} exceeds 65536 bytes".format(item.get("name")))

    decoded = json.loads(wire, object_pairs_hook=no_duplicate_object_pairs)
    if decoded != item.get("expected"):
        fail("control fixture {!r} does not match expected".format(item.get("name")))
    if set(decoded) != REQUIRED_ENVELOPE_FIELDS:
        fail("control fixture {!r} has the wrong envelope fields".format(item.get("name")))
    if decoded["version"] != 1:
        fail("control fixture {!r} is not protocol version 1".format(item.get("name")))
    if not isinstance(decoded["type"], str) or not 1 <= len(decoded["type"].encode("ascii")) <= 64:
        fail("control fixture {!r} has an invalid type".format(item.get("name")))
    if not isinstance(decoded["message_id"], str) or not 1 <= len(decoded["message_id"].encode("utf-8")) <= 64:
        fail("control fixture {!r} has an invalid message_id".format(item.get("name")))
    uuid.UUID(decoded["device_id"])
    for field_name in ("session_id", "turn_id", "stream_id", "epoch"):
        validate_u32(decoded[field_name], field_name)
    if not isinstance(decoded["payload"], dict):
        fail("control fixture {!r} payload must be an object".format(item.get("name")))


def decode_audio_header(header_bytes):
    values = HEADER.unpack(header_bytes)
    return {
        "magic": values[0].decode("ascii"),
        "version": values[1],
        "kind": values[2],
        "flags": values[3],
        "header_length_bytes": values[4],
        "audio_format": values[5],
        "channels": values[6],
        "sample_rate_hz": values[7],
        "payload_length_bytes": values[8],
        "sequence": values[9],
        "timestamp_us": values[10],
        "epoch": values[11],
        "device_id": str(uuid.UUID(bytes=values[12])),
        "session_id": values[13],
        "turn_id": values[14],
        "stream_id": values[15],
    }


def validate_audio_fixture(item):
    try:
        header_bytes = bytes.fromhex(item["header_hex"])
        payload_bytes = bytes.fromhex(item["payload_hex"])
        wire_bytes = bytes.fromhex(item["wire_hex"])
    except (KeyError, ValueError) as error:
        fail("audio fixture {!r} has invalid hex: {}".format(item.get("name"), error))

    if HEADER.size != 64 or len(header_bytes) != HEADER.size:
        fail("audio fixture {!r} header is not 64 bytes".format(item.get("name")))
    if wire_bytes != header_bytes + payload_bytes:
        fail("audio fixture {!r} wire_hex is not header + payload".format(item.get("name")))

    decoded = decode_audio_header(header_bytes)
    if decoded != item.get("header"):
        fail("audio fixture {!r} decoded header differs from expected".format(item.get("name")))
    if decoded["magic"] != "BPV1" or decoded["version"] != 1:
        fail("audio fixture {!r} has invalid magic/version".format(item.get("name")))
    if decoded["header_length_bytes"] != HEADER.size:
        fail("audio fixture {!r} has invalid header length".format(item.get("name")))
    if decoded["payload_length_bytes"] != len(payload_bytes) or len(payload_bytes) > 65536:
        fail("audio fixture {!r} has invalid payload length".format(item.get("name")))
    if decoded["audio_format"] != 1 or decoded["channels"] < 1:
        fail("audio fixture {!r} has invalid initial audio format".format(item.get("name")))
    if len(payload_bytes) % (decoded["channels"] * 2) != 0:
        fail("audio fixture {!r} payload is not whole S16_LE samples".format(item.get("name")))
    for field_name in ("epoch", "session_id", "turn_id", "stream_id"):
        validate_u32(decoded[field_name], field_name, allow_zero=False)

    sample_count = len(payload_bytes) // 2
    samples = list(struct.unpack("<{}h".format(sample_count), payload_bytes))
    if samples != item.get("payload_samples_s16le"):
        fail("audio fixture {!r} PCM samples differ from expected".format(item.get("name")))


def main():
    repository_root = Path(__file__).resolve().parents[1]
    fixture_path = repository_root / "protocol" / "fixtures" / "protocol-v1-golden.json"
    with fixture_path.open("r", encoding="utf-8") as fixture_file:
        document = json.load(fixture_file, object_pairs_hook=no_duplicate_object_pairs)

    if document.get("fixture_version") != 1:
        fail("unsupported fixture_version")

    controls = document.get("control_frames")
    audio = document.get("audio_frames")
    if not isinstance(controls, list) or not controls:
        fail("control_frames must be a non-empty array")
    if not isinstance(audio, list) or not audio:
        fail("audio_frames must be a non-empty array")

    for item in controls:
        validate_control_fixture(item)
    for item in audio:
        validate_audio_fixture(item)

    print(
        "validated {} control fixture(s) and {} audio fixture(s)".format(
            len(controls), len(audio)
        )
    )
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError, KeyError, UnicodeError) as error:
        print("protocol fixture validation failed: {}".format(error), file=sys.stderr)
        sys.exit(1)
