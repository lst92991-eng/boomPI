#include "boompi/protocol/envelope.h"

namespace boompi::protocol {
namespace {

bool IsLowerHex(const char value) {
  return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f');
}

bool IsCanonicalDeviceId(const std::string& device_id) {
  if (device_id.size() != kCanonicalDeviceIdBytes) {
    return false;
  }
  bool has_non_zero_hex_digit = false;
  for (std::size_t index = 0; index < device_id.size(); ++index) {
    const bool is_separator =
        index == 8U || index == 13U || index == 18U || index == 23U;
    if (is_separator ? device_id[index] != '-'
                     : !IsLowerHex(device_id[index])) {
      return false;
    }
    if (!is_separator && device_id[index] != '0') {
      has_non_zero_hex_digit = true;
    }
  }
  return has_non_zero_hex_digit;
}

}  // namespace

Status ValidateEnvelopeMetadata(const EnvelopeMetadata& metadata) {
  if (metadata.version != kProtocolVersion) {
    return Status::Error(StatusCode::kNotSupported,
                         "unsupported control protocol version");
  }
  if (metadata.type.empty() || metadata.type.size() > kMaximumMessageTypeBytes) {
    return Status::Error(StatusCode::kInvalidArgument,
                         "control message type is empty or too long");
  }
  for (const char value : metadata.type) {
    const auto byte = static_cast<unsigned char>(value);
    if (byte < 0x21U || byte > 0x7EU) {
      return Status::Error(StatusCode::kInvalidArgument,
                           "control message type must use visible ASCII");
    }
  }
  if (metadata.message_id.empty() ||
      metadata.message_id.size() > kMaximumMessageIdBytes) {
    return Status::Error(StatusCode::kInvalidArgument,
                         "control message id is empty or too long");
  }
  if (!IsCanonicalDeviceId(metadata.device_id)) {
    return Status::Error(StatusCode::kInvalidArgument,
                         "device id must be a canonical lowercase UUID");
  }
  if (metadata.frame_size_bytes == 0U ||
      metadata.frame_size_bytes > kMaximumControlFrameBytes) {
    return Status::Error(StatusCode::kInvalidArgument,
                         "control frame size is outside allowed bounds");
  }
  return Status::Ok();
}

}  // namespace boompi::protocol
