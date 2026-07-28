# boomPI Protocol v1

## 1. Status and scope

This document is the P1 wire-contract baseline shared by the RV1106 C++ client and the Go server. It defines framing, identifiers, bounds and cancellation semantics. The Go MVP implements the WSS and streaming subset listed in section 4.2. Discovery, pairing, device-token authentication, reconnect and flow credit are still pending.

Normative words `MUST`, `MUST NOT`, `SHOULD` and `MAY` are used deliberately. Protocol implementations must reject malformed lengths before allocation or state mutation.

## 2. Transports

- UDP port `17807`: unauthenticated LAN discovery only.
- TCP/WSS port `17806`: one persistent TLS connection carrying UTF-8 JSON text frames and binary PCM frames. The current server limits this to one device; it is not considered authenticated until pairing and device tokens are implemented.
- WebSocket ping/pong: default idle ping interval 10 seconds and dead-connection timeout 30 seconds; both are configurable but a finite deadline is mandatory.
- Application audio is real-time and is never retransmitted after reconnect.

UDP discovery is not a trust mechanism. An offer only supplies a candidate endpoint; first connection still requires the six-digit pairing flow and TLS SPKI confirmation described by the root development contract.

## 3. Identifier model

| Name | Wire type | Meaning |
| --- | --- | --- |
| `device_id` | canonical UUID / 16 raw bytes | Random persistent board identity; never derived from MAC |
| `session_id` | unsigned 32-bit | Connection/session scope; `0` only before session assignment |
| `turn_id` | unsigned 32-bit | User utterance scope; `0` only when no turn exists |
| `stream_id` | unsigned 32-bit | Audio/text stream scope; `0` only when no stream exists |
| `epoch` | unsigned 32-bit | Cancellation/reconnect generation; nonzero on active media |
| `message_id` | UTF-8 string, 1–64 bytes | Control-message correlation ID |
| `sequence` | unsigned 32-bit | Per-stream audio sequence number; wrap is an explicit discontinuity |

IDs are connection-scoped except `device_id`. A reconnect creates a new session and epoch. Messages from an older epoch must not mutate state.

## 4. JSON control envelope

Every WebSocket text frame is one complete UTF-8 JSON object with these top-level fields:

```json
{
  "version": 1,
  "type": "turn.start",
  "message_id": "msg-0042",
  "device_id": "00112233-4455-6677-8899-aabbccddeeff",
  "session_id": 16909060,
  "turn_id": 168496141,
  "stream_id": 287454020,
  "epoch": 7,
  "payload": {}
}
```

Rules:

- `version` is the JSON integer `1`, not the string `"1"`.
- All nine fields are present. A field not applicable to pre-session traffic uses numeric `0`; it is not omitted or encoded as `null`.
- Maximum text-frame size is 65,536 bytes after UTF-8 encoding.
- `type` is 1–64 visible ASCII bytes (`0x21`–`0x7e`). Receivers must not change state for an unknown type.
- `payload` is always an object. Individual message schemas must define their own required fields and bounds.
- Duplicate JSON object keys, invalid UTF-8, fractional/negative IDs and values outside unsigned 32-bit range are invalid.
- JSON member order and whitespace have no semantic meaning.

### 4.1 Initial message set

| Type | Direction | Required payload purpose |
| --- | --- | --- |
| `hello` | device → server | Development device authentication; IDs are zero before assignment |
| `hello.ack` | server → device | Assigned session/epoch and negotiated capabilities |
| `turn.start` | device → server | Manual-VAD utterance start and input audio format |
| `turn.commit` | device → server | End of utterance and total captured samples |
| `turn.cancel` | either | Abort the named input turn with a bounded reason code |
| `response.start` | server → device | Begin assistant response and identify text/audio streams |
| `response.text_delta` | server → device | Bounded UTF-8 text fragment for the current response |
| `response.audio_start` | server → device | Negotiated PCM format before binary downlink frames |
| `response.cancel` | device → server | Barge-in/user cancellation with last playback progress |
| `response.cancelled` | server → device | Cancellation acknowledgement; never reactivates playback |
| `response.done` | server → device | Normal response completion |
| `playback.progress` | device → server | `response_id`, `played_samples` and `played_ms` |
| `flow.credit` | device → server | Additional downlink audio capacity in milliseconds |
| `state.update` | either | Bounded product/network state update |
| `error` | either | Stable error code and a safe, non-secret diagnostic message |

Pairing, update and tool payload schemas will be added before those features are implemented. Implementations must not invent ad-hoc production payloads outside this document.

### 4.2 Implemented server MVP subset

The current Go server accepts one device on `wss://<host>:17806/ws`. Its first message must arrive within five seconds and must be `hello`, with a canonical persistent `device_id`, all numeric identifiers set to zero, and the exact development payload `{"device_token":"<environment-provisioned token>"}`. Unknown or missing hello payload fields are rejected. The server authenticates this token before opening a provider session. `hello.ack` assigns a nonzero `session_id`, starts at epoch 1, and advertises 16 kHz input, 24 kHz output and 20 ms input frames.

For each manual-VAD turn, the device sends `turn.start` with nonzero `session_id`, `turn_id`, `stream_id` and monotonic `epoch`; its payload is `{"sample_rate_hz":16000}`. Binary uplink frames then use those identifiers and consecutive sequence numbers starting at zero. `turn.commit` ends the input. The server may return `response.start`, `response.text_delta`, `response.audio_start`, binary 24 kHz PCM, and `response.done`. `turn.cancel` and `response.cancel` retire the active epoch and return `response.cancelled`.

This subset deliberately omits discovery, pairing, per-device token provisioning, conversation persistence, tools and flow credit. The shared development token is only a fail-closed guard against unauthenticated LAN clients triggering provider calls; it is not a replacement for pairing. Until pairing lands, the generated stable SPKI digest and manually provisioned token must be treated as development information rather than a complete production trust flow.

## 5. Binary audio frame

Each WebSocket binary message contains exactly one header followed by one PCM payload:

```text
64-byte v1 header (network byte order) | PCM payload (S16_LE samples)
```

All multibyte header integers are unsigned big-endian. PCM sample bytes remain little-endian because the negotiated format is `PCM_S16LE`.

### 5.1 Fixed 64-byte header

| Offset | Width | Field | v1 rule |
| ---: | ---: | --- | --- |
| 0 | 4 | `magic` | ASCII `BPV1` |
| 4 | 1 | `version` | `1` |
| 5 | 1 | `kind` | `1` uplink PCM, `2` downlink PCM |
| 6 | 2 | `flags` | bit mask below |
| 8 | 2 | `header_length_bytes` | exactly `64` in v1 |
| 10 | 1 | `audio_format` | `1` = PCM S16_LE |
| 11 | 1 | `channels` | `1` for the initial client/server stream |
| 12 | 4 | `sample_rate_hz` | explicit negotiated rate |
| 16 | 4 | `payload_length_bytes` | bytes following the header |
| 20 | 4 | `sequence` | per-stream sequence |
| 24 | 8 | `timestamp_us` | sender source-monotonic timestamp; clocks are not compared across hosts |
| 32 | 4 | `epoch` | active generation |
| 36 | 16 | `device_id` | RFC 4122 UUID bytes in network order |
| 52 | 4 | `session_id` | active nonzero session |
| 56 | 4 | `turn_id` | active nonzero turn |
| 60 | 4 | `stream_id` | active nonzero stream |

Flags:

- `0x0001 START`: first payload in the stream.
- `0x0002 END`: final payload in the stream.
- `0x0004 DISCONTINUITY`: samples were lost or timing continuity is broken.
- All other bits are reserved and must be zero in v1.

Validation order:

1. Require at least 64 received bytes.
2. Validate magic, version, kind, header length, format and flags.
3. Require `payload_length_bytes` to equal the actual bytes after the header and to be at most 65,536.
4. Require payload length to be divisible by `channels * 2` for S16_LE.
5. Validate the rate against the stream negotiated by the preceding control message.
6. Validate nonzero active IDs and the expected epoch before allocating or publishing a frame.

Header parsing and active-stream authorization are separate checks. A parser first
validates the fixed layout and structural bounds above; the session layer must then
match `kind`, negotiated sample rate, device/session/turn/stream IDs, `epoch`, and
the expected `sequence` against its current stream context. A structurally valid
header is not sufficient to reactivate an old or different stream. Sequence wrap or
reset must be represented as an explicit discontinuity and handled by the owning
session actor.

Producers target 20 ms frames. The final frame may be shorter; implementations must not assume payload length from the target duration without checking the header.

## 6. Turn and response sequence

```text
device                         server
  | -------- hello ----------> |
  | <------ hello.ack -------- |
  | ------- turn.start ------> |
  | ===== binary uplink =====> |
  | ------- turn.commit -----> |
  | <---- response.start ----- |
  | <--- text/audio_start ---- |
  | <==== binary downlink ==== |
  | ---- playback.progress --> |
  | <----- response.done ----- |
```

Manual VAD on the device decides `turn.start`/`turn.commit`. Network loss cancels the current turn and response. A reconnect never resumes a partially transmitted utterance.

### 6.1 Barge-in

On confirmed user speech during playback, the device must locally stop/flush the old response and increment its response generation before waiting for the server. It sends `response.cancel` with the last confirmed playback position, then starts a new user turn including AEC-clean pre-roll.

Late text/audio/cancel acknowledgements from the old generation are discarded. If reliable text/audio alignment is unavailable, the server deletes the whole interrupted assistant turn from model context instead of retaining an answer the user did not hear.

## 7. Flow control

- The initial device TTS target is 180 ms, adaptive from 120–400 ms, with a hard local limit of 1.5 seconds.
- `flow.credit` grants additional downlink capacity in milliseconds. The server must pause board-bound audio when no credit remains.
- Server provider-to-board buffering is also bounded. If provider and board backpressure cannot be reconciled without exceeding bounds, cancel the response and report congestion; do not drop middle PCM or allocate an unbounded queue.
- Uplink buffering is bounded to 800 ms. Overflow cancels the turn and does not cause stale audio replay.

## 8. Discovery baseline

Discovery datagrams are UTF-8 JSON and must remain below 1,200 bytes. A request contains `version`, type `discover`, a random nonce and `device_id`. An offer echoes the nonce and contains type `offer`, `server_id`, `wss_port`, protocol versions and capabilities. It must never contain a device token, certificate private material, Wi-Fi credentials or provider credentials.

Because discovery is unauthenticated, the device treats all offers as hints. A cached SPKI pin is still mandatory for a paired server; a new server key requires explicit re-pairing.

## 9. Compatibility

- A v1 parser accepts only major version `1` unless capability negotiation explicitly adds another version.
- New optional fields may be added to payload objects, but existing meanings cannot change.
- New message types require documentation and capability negotiation before use.
- New binary-header fields require a later negotiated header/version; v1 header length remains exactly 64.
- Provider-specific Qwen events never cross this protocol boundary.

## 10. Shared fixture

`protocol/fixtures/protocol-v1-golden.json` is the cross-language authority for the initial control envelope and binary header. C++ and Go tests must decode the same fixture or compare their encoders against its exact wire bytes. `scripts/verify_protocol_fixtures.py` independently checks its structure using only the Python standard library.
