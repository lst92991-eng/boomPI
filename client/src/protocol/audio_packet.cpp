#include "boompi/protocol/audio_packet.h"

#include <algorithm>

namespace boompi::protocol {
namespace {

constexpr std::array<std::uint8_t, 4> kMagic{{'B', 'P', 'V', '1'}};

void WriteU16(const std::uint16_t value, std::uint8_t* const output) {
  output[0] = static_cast<std::uint8_t>((value >> 8U) & 0xFFU);
  output[1] = static_cast<std::uint8_t>(value & 0xFFU);
}

void WriteU32(const std::uint32_t value, std::uint8_t* const output) {
  output[0] = static_cast<std::uint8_t>((value >> 24U) & 0xFFU);
  output[1] = static_cast<std::uint8_t>((value >> 16U) & 0xFFU);
  output[2] = static_cast<std::uint8_t>((value >> 8U) & 0xFFU);
  output[3] = static_cast<std::uint8_t>(value & 0xFFU);
}

void WriteU64(const std::uint64_t value, std::uint8_t* const output) {
  for (std::size_t index = 0; index < 8U; ++index) {
    const auto shift = static_cast<unsigned>((7U - index) * 8U);
    output[index] = static_cast<std::uint8_t>((value >> shift) & 0xFFU);
  }
}

std::uint16_t ReadU16(const std::uint8_t* const input) {
  return static_cast<std::uint16_t>(
      (static_cast<std::uint16_t>(input[0]) << 8U) |
      static_cast<std::uint16_t>(input[1]));
}

std::uint32_t ReadU32(const std::uint8_t* const input) {
  return (static_cast<std::uint32_t>(input[0]) << 24U) |
         (static_cast<std::uint32_t>(input[1]) << 16U) |
         (static_cast<std::uint32_t>(input[2]) << 8U) |
         static_cast<std::uint32_t>(input[3]);
}

std::uint64_t ReadU64(const std::uint8_t* const input) {
  std::uint64_t value = 0U;
  for (std::size_t index = 0; index < 8U; ++index) {
    value = (value << 8U) | static_cast<std::uint64_t>(input[index]);
  }
  return value;
}

bool HasNonZeroUuid(const std::array<std::uint8_t, 16>& uuid) {
  return std::any_of(uuid.begin(), uuid.end(),
                     [](const std::uint8_t value) { return value != 0U; });
}

}  // namespace

Status ValidateAudioPacketHeader(const AudioPacketHeader& header) {
  if (header.version != kProtocolVersion) {
    return Status::Error(StatusCode::kNotSupported,
                         "unsupported audio packet protocol version");
  }
  if (header.kind != AudioPacketKind::kUplink &&
      header.kind != AudioPacketKind::kDownlink) {
    return Status::Error(StatusCode::kInvalidArgument,
                         "invalid audio packet kind");
  }
  if (header.audio_format != WireAudioFormat::kPcmS16Le) {
    return Status::Error(StatusCode::kNotSupported,
                         "unsupported wire audio format");
  }
  if ((header.flags & static_cast<std::uint16_t>(~kKnownAudioPacketFlags)) !=
      0U) {
    return Status::Error(StatusCode::kInvalidArgument,
                         "audio packet contains reserved flags");
  }
  if (header.channels != 1U) {
    return Status::Error(StatusCode::kInvalidArgument,
                         "protocol v1 audio must be mono");
  }
  if (header.sample_rate_hz < 8000U || header.sample_rate_hz > 96000U) {
    return Status::Error(StatusCode::kInvalidArgument,
                         "wire sample rate must be between 8000 and 96000 Hz");
  }
  if (header.payload_length_bytes == 0U ||
      header.payload_length_bytes > kMaximumAudioPayloadBytes) {
    return Status::Error(StatusCode::kInvalidArgument,
                         "audio payload length is outside allowed bounds");
  }
  const auto frame_alignment =
      static_cast<std::uint32_t>(header.channels) * 2U;
  if (header.payload_length_bytes % frame_alignment != 0U) {
    return Status::Error(StatusCode::kInvalidArgument,
                         "audio payload is not aligned to PCM samples");
  }
  if (header.epoch == 0U || header.session_id == 0U ||
      header.turn_id == 0U || header.stream_id == 0U ||
      !HasNonZeroUuid(header.device_uuid)) {
    return Status::Error(StatusCode::kInvalidArgument,
                         "audio identity fields must be non-zero");
  }
  return Status::Ok();
}

Status ValidateAudioPacketContext(const AudioPacketHeader& header,
                                  const AudioStreamContext& context) {
  const auto header_status = ValidateAudioPacketHeader(header);
  if (!header_status.ok()) {
    return header_status;
  }
  if (header.kind != context.kind) {
    return Status::Error(StatusCode::kFailedPrecondition,
                         "audio packet kind does not match stream context");
  }
  if (header.sample_rate_hz != context.sample_rate_hz) {
    return Status::Error(StatusCode::kFailedPrecondition,
                         "audio sample rate does not match stream context");
  }
  if (header.device_uuid != context.device_uuid) {
    return Status::Error(StatusCode::kFailedPrecondition,
                         "audio device id does not match stream context");
  }
  if (header.epoch != context.epoch) {
    return Status::Error(StatusCode::kFailedPrecondition,
                         "audio epoch does not match stream context");
  }
  if (header.session_id != context.session_id) {
    return Status::Error(StatusCode::kFailedPrecondition,
                         "audio session does not match stream context");
  }
  if (header.turn_id != context.turn_id) {
    return Status::Error(StatusCode::kFailedPrecondition,
                         "audio turn does not match stream context");
  }
  if (header.stream_id != context.stream_id) {
    return Status::Error(StatusCode::kFailedPrecondition,
                         "audio stream id does not match stream context");
  }
  if (header.sequence != context.expected_sequence) {
    return Status::Error(StatusCode::kFailedPrecondition,
                         "audio sequence does not match stream context");
  }
  return Status::Ok();
}

Status EncodeAudioPacketHeader(
    const AudioPacketHeader& header,
    std::array<std::uint8_t, kAudioPacketHeaderSize>* const output) {
  if (output == nullptr) {
    return Status::Error(StatusCode::kInvalidArgument,
                         "audio header output must not be null");
  }
  const auto status = ValidateAudioPacketHeader(header);
  if (!status.ok()) {
    return status;
  }

  output->fill(0U);
  std::copy(kMagic.begin(), kMagic.end(), output->begin());
  (*output)[4] = header.version;
  (*output)[5] = static_cast<std::uint8_t>(header.kind);
  WriteU16(header.flags, output->data() + 6U);
  WriteU16(static_cast<std::uint16_t>(kAudioPacketHeaderSize),
           output->data() + 8U);
  (*output)[10] = static_cast<std::uint8_t>(header.audio_format);
  (*output)[11] = header.channels;
  WriteU32(header.sample_rate_hz, output->data() + 12U);
  WriteU32(header.payload_length_bytes, output->data() + 16U);
  WriteU32(header.sequence, output->data() + 20U);
  WriteU64(header.monotonic_timestamp_us, output->data() + 24U);
  WriteU32(header.epoch, output->data() + 32U);
  std::copy(header.device_uuid.begin(), header.device_uuid.end(),
            output->begin() + 36U);
  WriteU32(header.session_id, output->data() + 52U);
  WriteU32(header.turn_id, output->data() + 56U);
  WriteU32(header.stream_id, output->data() + 60U);
  return Status::Ok();
}

Status DecodeAudioPacketHeader(const std::uint8_t* const data,
                               const std::size_t data_size,
                               AudioPacketHeader* const output) {
  if (data == nullptr || output == nullptr) {
    return Status::Error(StatusCode::kInvalidArgument,
                         "audio header input and output must not be null");
  }
  if (data_size < kAudioPacketHeaderSize) {
    return Status::Error(StatusCode::kInvalidArgument,
                         "audio header is truncated");
  }
  if (!std::equal(kMagic.begin(), kMagic.end(), data)) {
    return Status::Error(StatusCode::kInvalidArgument,
                         "audio header magic does not match BPV1");
  }
  if (ReadU16(data + 8U) != kAudioPacketHeaderSize) {
    return Status::Error(StatusCode::kInvalidArgument,
                         "audio header length is not 64 bytes");
  }

  AudioPacketHeader decoded{};
  decoded.version = data[4];
  decoded.kind = static_cast<AudioPacketKind>(data[5]);
  decoded.flags = ReadU16(data + 6U);
  decoded.audio_format = static_cast<WireAudioFormat>(data[10]);
  decoded.channels = data[11];
  decoded.sample_rate_hz = ReadU32(data + 12U);
  decoded.payload_length_bytes = ReadU32(data + 16U);
  decoded.sequence = ReadU32(data + 20U);
  decoded.monotonic_timestamp_us = ReadU64(data + 24U);
  decoded.epoch = ReadU32(data + 32U);
  std::copy(data + 36U, data + 52U, decoded.device_uuid.begin());
  decoded.session_id = ReadU32(data + 52U);
  decoded.turn_id = ReadU32(data + 56U);
  decoded.stream_id = ReadU32(data + 60U);

  const auto status = ValidateAudioPacketHeader(decoded);
  if (!status.ok()) {
    return status;
  }
  const auto expected_size = kAudioPacketHeaderSize +
                             static_cast<std::size_t>(
                                 decoded.payload_length_bytes);
  if (data_size != expected_size) {
    return Status::Error(StatusCode::kInvalidArgument,
                         "audio payload length does not match frame size");
  }
  *output = decoded;
  return Status::Ok();
}

}  // namespace boompi::protocol
