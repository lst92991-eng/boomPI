#ifndef BOOMPI_PROTOCOL_ENVELOPE_H_
#define BOOMPI_PROTOCOL_ENVELOPE_H_

#include <cstddef>
#include <cstdint>
#include <string>

#include "boompi/event/status.h"
#include "boompi/protocol/audio_packet.h"

namespace boompi::protocol {

constexpr std::size_t kMaximumControlFrameBytes = 64U * 1024U;
constexpr std::size_t kMaximumMessageTypeBytes = 64U;
constexpr std::size_t kMaximumMessageIdBytes = 64U;
constexpr std::size_t kCanonicalDeviceIdBytes = 36U;

struct EnvelopeMetadata final {
  std::uint8_t version{kProtocolVersion};
  std::string type;
  std::string message_id;
  std::string device_id;
  std::uint32_t session_id{0};
  std::uint32_t turn_id{0};
  std::uint32_t stream_id{0};
  std::uint32_t epoch{0};
  std::size_t frame_size_bytes{0};
};

Status ValidateEnvelopeMetadata(const EnvelopeMetadata& metadata);

}  // namespace boompi::protocol

#endif  // BOOMPI_PROTOCOL_ENVELOPE_H_
