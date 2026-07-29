#ifndef BOOMPI_PROTOCOL_SERVER_CONTROL_H_
#define BOOMPI_PROTOCOL_SERVER_CONTROL_H_

#include <cstdint>
#include <string>
#include <string_view>

#include "boompi/event/status.h"
#include "boompi/protocol/envelope.h"

namespace boompi::protocol {

enum class ServerControlType : std::uint8_t {
  kHelloAck = 0,
  kResponseStart,
  kResponseTextDelta,
  kResponseAudioStart,
  kResponseCancelled,
  kResponseDone,
  kError,
};

struct ServerControlMessage final {
  ServerControlType type{ServerControlType::kHelloAck};
  EnvelopeMetadata envelope;

  std::uint32_t input_sample_rate_hz{0U};
  std::uint32_t output_sample_rate_hz{0U};
  std::uint32_t input_frame_ms{0U};

  std::string response_id;
  std::string text;
  std::uint32_t sample_rate_hz{0U};
  std::string cancellation_reason;

  std::string error_code;
  std::string error_message;
};

Status DecodeServerControl(std::string_view control_json,
                           std::string_view expected_device_id,
                           ServerControlMessage* output);

}  // namespace boompi::protocol

#endif  // BOOMPI_PROTOCOL_SERVER_CONTROL_H_
