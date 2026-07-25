#ifndef BOOMPI_PROTOCOL_AUDIO_PACKET_H_
#define BOOMPI_PROTOCOL_AUDIO_PACKET_H_

#include <array>
#include <cstddef>
#include <cstdint>

#include "boompi/event/status.h"

namespace boompi::protocol {

constexpr std::uint8_t kProtocolVersion = 1U;
constexpr std::size_t kAudioPacketHeaderSize = 64U;
constexpr std::uint32_t kMaximumAudioPayloadBytes = 64U * 1024U;
constexpr std::uint16_t kKnownAudioPacketFlags = 0x0007U;

enum class AudioPacketKind : std::uint8_t {
  kUplink = 1U,
  kDownlink = 2U,
};

enum class WireAudioFormat : std::uint8_t {
  kPcmS16Le = 1U,
};

struct AudioPacketHeader final {
  std::uint8_t version{kProtocolVersion};
  AudioPacketKind kind{AudioPacketKind::kUplink};
  std::uint16_t flags{0};
  WireAudioFormat audio_format{WireAudioFormat::kPcmS16Le};
  std::uint8_t channels{1};
  std::uint32_t sample_rate_hz{16000};
  std::uint32_t payload_length_bytes{0};
  std::uint32_t sequence{0};
  std::uint64_t monotonic_timestamp_us{0};
  std::uint32_t epoch{0};
  std::array<std::uint8_t, 16> device_uuid{};
  std::uint32_t session_id{0};
  std::uint32_t turn_id{0};
  std::uint32_t stream_id{0};
};

struct AudioStreamContext final {
  AudioPacketKind kind{AudioPacketKind::kUplink};
  std::uint32_t sample_rate_hz{16000};
  std::array<std::uint8_t, 16> device_uuid{};
  std::uint32_t epoch{0};
  std::uint32_t session_id{0};
  std::uint32_t turn_id{0};
  std::uint32_t stream_id{0};
  std::uint32_t expected_sequence{0};
};

Status ValidateAudioPacketHeader(const AudioPacketHeader& header);
Status ValidateAudioPacketContext(const AudioPacketHeader& header,
                                  const AudioStreamContext& context);
Status EncodeAudioPacketHeader(
    const AudioPacketHeader& header,
    std::array<std::uint8_t, kAudioPacketHeaderSize>* output);
Status DecodeAudioPacketHeader(const std::uint8_t* data,
                               std::size_t data_size,
                               AudioPacketHeader* output);

}  // namespace boompi::protocol

#endif  // BOOMPI_PROTOCOL_AUDIO_PACKET_H_
