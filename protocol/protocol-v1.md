# boomPI Protocol v1

## 1. Status and scope

This document is the P1 wire-contract baseline shared by the RV1106 C++ client and the Go server. It defines framing, identifiers, bounds and cancellation semantics. The Go MVP implements the WSS and streaming subset listed in section 4.2, the fixed discovery exchange in section 8, and shared development-token authentication. Pairing, per-device token provisioning and flow credit are still pending. The teaching client already performs bounded automatic reconnect: it discards the old turn and media, opens a new session/epoch and never retransmits real-time audio.

Normative words `MUST`, `MUST NOT`, `SHOULD` and `MAY` are used deliberately. Protocol implementations must reject malformed lengths before allocation or state mutation.

## 2. Transports

- UDP port `17807`: unauthenticated LAN discovery only.
- TCP/WSS port `17806`: one persistent TLS connection carrying UTF-8 JSON text frames and binary PCM frames. The current server limits this to one device and authenticates it with a shared development token; pairing and per-device token provisioning are not yet implemented.
- WebSocket ping/pong: the server sends a periodic Ping every 1–10 seconds (10 seconds by default), and requires the Pong timeout to cover at least three Ping intervals without exceeding 30 seconds. The teaching client automatically replies with Pong and declares the server unavailable after 30 seconds without an inbound Ping. Receiving a Pong refreshes the server deadline but never suppresses its next periodic Ping.
- Application audio is real-time and is never retransmitted after reconnect.

UDP discovery is not a trust mechanism. A response supplies only a candidate endpoint and its TLS SPKI digest. The current teaching build checks the configured shared token and SPKI pin; production pairing and per-device provisioning remain future work.

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
  "payload": {"sample_rate_hz": 16000}
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

| Type | Direction | Exact v1 payload schema |
| --- | --- | --- |
| `hello` | device → server | `{"device_token":"<32..256 visible ASCII bytes>"}` |
| `hello.ack` | server → device | `{"input_sample_rate_hz":16000,"output_sample_rate_hz":24000,"input_frame_ms":20}` |
| `turn.start` | device → server | `{"sample_rate_hz":16000}` |
| `turn.commit` | device → server | `{}`; binary END/sequence defines the sample boundary |
| `turn.cancel` | device → server | `{}` |
| `response.start` | server → device | `{"response_id":"<1..128 bytes>"}` |
| `response.text_delta` | server → device | `{"response_id":"<1..128 bytes>","text":"<1..4096 bytes>"}` |
| `response.audio_start` | server → device | `{"response_id":"<1..128 bytes>","sample_rate_hz":24000}` |
| `response.cancel` | device → server | `{}` |
| `response.cancelled` | server → device | `{"reason":"<1..64 bytes>"}` |
| `response.done` | server → device | `{"response_id":"<1..128 bytes>"}` |
| `error` | server → device | `{"code":"<1..64 bytes>","message":"<1..512 bytes>"}` |

`playback.progress`, `flow.credit` and `state.update` are reserved future message types. The current teaching v1 does not send, accept or negotiate them.

Pairing, update and tool payload schemas will be added before those features are implemented. Implementations must not invent ad-hoc production payloads outside this document.

### 4.2 Implemented server MVP subset

The current Go server accepts one device on `wss://<host>:17806/ws`. Its first message must arrive within five seconds and must be `hello`, with a canonical persistent `device_id`, all numeric identifiers set to zero, and the exact development payload `{"device_token":"<environment-provisioned token>"}`. Unknown or missing hello payload fields are rejected. The server authenticates this token before opening a provider session. `hello.ack` assigns a nonzero `session_id`, starts at epoch 1, and advertises 16 kHz input, 24 kHz output and 20 ms input frames.

For each manual-VAD turn, the device sends `turn.start` with nonzero `session_id`, `turn_id`, `stream_id` and monotonic `epoch`; its payload is `{"sample_rate_hz":16000}`. Binary uplink frames then use those identifiers and consecutive sequence numbers starting at zero. `turn.commit`, `turn.cancel` and `response.cancel` each use the exact payload `{}`; the binary END flag and sequence identify the final uploaded sample boundary. `turn.commit` ends the input. The server may return `response.start`, `response.text_delta`, `response.audio_start`, binary 24 kHz PCM, and `response.done`. `turn.cancel` and `response.cancel` retire the active epoch and return `response.cancelled`.

This subset deliberately omits pairing, per-device token provisioning, conversation persistence, tools and flow credit. Discovery uses the fixed teaching exchange in section 8. The shared development token is only a fail-closed guard against unauthenticated LAN clients triggering provider calls; it is not a replacement for pairing. Until pairing lands, the generated stable SPKI digest and manually provisioned token must be treated as development information rather than a complete production trust flow.

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

A sender SHOULD release each complete 20 ms downlink frame without waiting for a later provider event. If the sender learns that the stream ended only after every complete frame has already been sent, it MAY send one silent S16_LE sample in a new, sequence-contiguous packet carrying `END`. The packet remains ordinary non-empty PCM and the receiver processes its declared `payload_length_bytes`; zero-length END packets are not valid in v1. A real partial tail, when present, carries `END` directly and no silent marker is added.

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
  | <----- response.done ----- |
```

Manual VAD on the device decides `turn.start`/`turn.commit`. Network loss cancels the current turn and response. A reconnect never resumes a partially transmitted utterance.

### 6.1 Barge-in

On confirmed user speech during playback or the local ALSA drain tail, the device locally stops and flushes the old response, then sends `response.cancel` with exact payload `{}`. The current protocol does not report a playback position: after `response.cancelled`, the server deletes the whole interrupted assistant turn from model context. The device starts the next epoch only after cancellation acknowledgement and acoustic admission, including its AEC-clean pre-roll.

Late text/audio/cancel acknowledgements from the old generation are discarded. If reliable text/audio alignment is unavailable, the server deletes the whole interrupted assistant turn from model context instead of retaining an answer the user did not hear.

## 7. Flow control

- The device starts playback after 180 ms of queued TTS (or earlier for a completed short response) and then consumes 20 ms frames at wire cadence. Its fixed local ring has a hard 1.5-second limit; v1 does not adapt this target.
- `flow.credit` is not implemented or negotiated in v1. The server instead uses fixed bounded provider, actor and transport queues and a paced 20 ms downlink.
- If provider and board backpressure cannot be reconciled within those bounds, the current turn is cancelled and reported as congestion; neither side drops middle PCM or allocates an unbounded queue.
- Uplink buffering is bounded to 800 ms. Overflow cancels the turn and does not cause stale audio replay.

## 8. Discovery baseline

The request payload is exactly the UTF-8 text `BOOMPI_DISCOVER_V1`, with no JSON wrapper or trailing newline. The server ignores every other datagram.

The response payload is exactly one space-delimited UTF-8 line, also without a trailing newline:

```text
BOOMPI_SERVER_V1 <wss_port> <spki_sha256_base64>
```

`wss_port` is a decimal integer from 1 through 65535. `spki_sha256_base64` is standard Base64 encoding of the 32-byte SHA-256 digest of the server certificate's SubjectPublicKeyInfo. The device uses the UDP source address as the candidate WSS host. Neither request nor response contains a device token, certificate private material, Wi-Fi credentials or provider credentials.

Because discovery is unauthenticated, the device treats every response as a hint. It must validate the advertised SPKI digest during TLS setup; a changed server key requires explicit user approval or a future re-pairing flow.

## 9. Compatibility

- A v1 parser accepts only major version `1` unless capability negotiation explicitly adds another version.
- The v1 envelope and every v1 payload object use exact documented schemas. Receivers reject unknown or duplicate fields.
- Adding an optional payload field requires a negotiated later version or capability; existing v1 meanings cannot change.
- New message types require documentation and capability negotiation before use.
- New binary-header fields require a later negotiated header/version; v1 header length remains exactly 64.
- Provider-specific Qwen events never cross this protocol boundary.

## 10. Shared fixture

`protocol/fixtures/protocol-v1-golden.json` is the cross-language authority for the initial control envelope and binary header. C++ and Go tests must decode the same fixture or compare their encoders against its exact wire bytes. `scripts/verify_protocol_fixtures.py` independently checks its structure using only the Python standard library.
