#include "boompi/protocol/server_control.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace boompi::protocol {
namespace {

constexpr std::size_t kMaximumJsonDepth = 16U;
constexpr std::size_t kMaximumJsonNodes = 256U;
constexpr std::size_t kMaximumJsonKeyBytes = 64U;
constexpr std::size_t kMaximumResponseIdBytes = 128U;
constexpr std::size_t kMaximumTextDeltaBytes = 4096U;
constexpr std::size_t kMaximumCancellationReasonBytes = 64U;
constexpr std::size_t kMaximumErrorCodeBytes = 64U;
constexpr std::size_t kMaximumErrorMessageBytes = 512U;

bool IsValidUtf8(const std::string_view input) {
  for (std::size_t index = 0U; index < input.size();) {
    const auto first = static_cast<std::uint8_t>(input[index]);
    if (first <= 0x7FU) {
      ++index;
      continue;
    }

    std::size_t continuation_count = 0U;
    std::uint32_t code_point = 0U;
    std::uint32_t minimum_code_point = 0U;
    if ((first & 0xE0U) == 0xC0U) {
      continuation_count = 1U;
      code_point = first & 0x1FU;
      minimum_code_point = 0x80U;
    } else if ((first & 0xF0U) == 0xE0U) {
      continuation_count = 2U;
      code_point = first & 0x0FU;
      minimum_code_point = 0x800U;
    } else if ((first & 0xF8U) == 0xF0U) {
      continuation_count = 3U;
      code_point = first & 0x07U;
      minimum_code_point = 0x10000U;
    } else {
      return false;
    }
    if (continuation_count > input.size() - index - 1U) {
      return false;
    }
    for (std::size_t offset = 1U; offset <= continuation_count; ++offset) {
      const auto byte = static_cast<std::uint8_t>(input[index + offset]);
      if ((byte & 0xC0U) != 0x80U) {
        return false;
      }
      code_point = (code_point << 6U) | (byte & 0x3FU);
    }
    if (code_point < minimum_code_point || code_point > 0x10FFFFU ||
        (code_point >= 0xD800U && code_point <= 0xDFFFU)) {
      return false;
    }
    index += continuation_count + 1U;
  }
  return true;
}

bool IsVisibleAscii(const std::string& value, const std::size_t maximum_bytes) {
  if (value.empty() || value.size() > maximum_bytes) {
    return false;
  }
  return std::all_of(value.begin(), value.end(), [](const char current) {
    const auto byte = static_cast<unsigned char>(current);
    return byte >= 0x21U && byte <= 0x7EU;
  });
}

class JsonCursor final {
 public:
  explicit JsonCursor(const std::string_view input) : input_(input) {}

  bool ParseTopLevel(std::string_view expected_device_id,
                     ServerControlMessage* const message,
                     std::string_view* const payload_json) {
    if (!EnterNode(1U) || !Expect('{', "control must be a JSON object")) {
      return false;
    }
    std::array<bool, 9U> seen{};
    std::uint32_t version = 0U;
    if (Try('}')) {
      return Fail(StatusCode::kInvalidArgument,
                  "control is missing required fields");
    }
    for (;;) {
      std::string key;
      if (!ParseString(&key, kMaximumJsonKeyBytes) ||
          !Expect(':', "control field needs a colon")) {
        return false;
      }
      const int field = TopLevelFieldIndex(key);
      if (field < 0) {
        return Fail(StatusCode::kInvalidArgument,
                    "control contains an unknown top-level field");
      }
      const auto field_index = static_cast<std::size_t>(field);
      if (seen[field_index]) {
        return Fail(StatusCode::kInvalidArgument,
                    "control contains a duplicate top-level field");
      }
      seen[field_index] = true;

      switch (field) {
        case 0:
          if (!ParseUint32(&version, 2U)) {
            return false;
          }
          break;
        case 1:
          if (!EnterNode(2U) ||
              !ParseString(&message->envelope.type,
                           kMaximumMessageTypeBytes)) {
            return false;
          }
          break;
        case 2:
          if (!EnterNode(2U) ||
              !ParseString(&message->envelope.message_id,
                           kMaximumMessageIdBytes)) {
            return false;
          }
          break;
        case 3:
          if (!EnterNode(2U) ||
              !ParseString(&message->envelope.device_id,
                           kCanonicalDeviceIdBytes)) {
            return false;
          }
          break;
        case 4:
          if (!ParseUint32(&message->envelope.session_id, 2U)) {
            return false;
          }
          break;
        case 5:
          if (!ParseUint32(&message->envelope.turn_id, 2U)) {
            return false;
          }
          break;
        case 6:
          if (!ParseUint32(&message->envelope.stream_id, 2U)) {
            return false;
          }
          break;
        case 7:
          if (!ParseUint32(&message->envelope.epoch, 2U)) {
            return false;
          }
          break;
        case 8: {
          Space();
          const std::size_t payload_start = position_;
          if (position_ >= input_.size() || input_[position_] != '{') {
            return Fail(StatusCode::kInvalidArgument,
                        "control payload must be an object");
          }
          if (!SkipValue(2U)) {
            return false;
          }
          *payload_json = input_.substr(payload_start, position_ - payload_start);
          break;
        }
        default:
          return Fail(StatusCode::kInternal, "invalid control field index");
      }

      if (Try('}')) {
        break;
      }
      if (!Expect(',', "control fields must be comma-separated")) {
        return false;
      }
    }
    Space();
    if (position_ != input_.size()) {
      return Fail(StatusCode::kInvalidArgument,
                  "control contains trailing data");
    }
    if (std::find(seen.begin(), seen.end(), false) != seen.end()) {
      return Fail(StatusCode::kInvalidArgument,
                  "control is missing a required field");
    }
    if (version != static_cast<std::uint32_t>(kProtocolVersion)) {
      return Fail(StatusCode::kNotSupported,
                  "unsupported control protocol version");
    }
    message->envelope.version = static_cast<std::uint8_t>(version);
    message->envelope.frame_size_bytes = input_.size();
    const Status metadata_status =
        ValidateEnvelopeMetadata(message->envelope);
    if (!metadata_status.ok()) {
      return Fail(metadata_status.code(), metadata_status.message());
    }
    if (message->envelope.device_id != expected_device_id) {
      return Fail(StatusCode::kFailedPrecondition,
                  "control device id does not match the expected device");
    }
    return ParseType(message->envelope.type, &message->type);
  }

  bool ParsePayload(const ServerControlType type,
                    ServerControlMessage* const message) {
    if (!EnterNode(1U) || !Expect('{', "control payload must be an object")) {
      return false;
    }
    std::uint8_t seen_mask = 0U;
    if (!Try('}')) {
      for (;;) {
        std::string key;
        if (!ParseString(&key, kMaximumJsonKeyBytes) ||
            !Expect(':', "payload field needs a colon")) {
          return false;
        }
        const int field = PayloadFieldIndex(type, key);
        if (field < 0) {
          return Fail(StatusCode::kInvalidArgument,
                      "payload contains an unknown field");
        }
        const auto bit = static_cast<std::uint8_t>(1U << field);
        if ((seen_mask & bit) != 0U) {
          return Fail(StatusCode::kInvalidArgument,
                      "payload contains a duplicate field");
        }
        seen_mask = static_cast<std::uint8_t>(seen_mask | bit);
        if (!ParsePayloadValue(type, field, message)) {
          return false;
        }
        if (Try('}')) {
          break;
        }
        if (!Expect(',', "payload fields must be comma-separated")) {
          return false;
        }
      }
    }
    Space();
    if (position_ != input_.size()) {
      return Fail(StatusCode::kInvalidArgument,
                  "payload contains trailing data");
    }
    const std::uint8_t expected_mask =
        type == ServerControlType::kHelloAck
            ? 0x07U
            : (type == ServerControlType::kResponseTextDelta ||
                       type == ServerControlType::kResponseAudioStart ||
                       type == ServerControlType::kError
                   ? 0x03U
                   : 0x01U);
    if (seen_mask != expected_mask) {
      return Fail(StatusCode::kInvalidArgument,
                  "payload is missing a required field");
    }
    return ValidatePayloadStrings(type, *message);
  }

  Status status() const { return status_; }

 private:
  static int TopLevelFieldIndex(const std::string& key) {
    constexpr std::array<const char*, 9U> fields{
        {"version", "type", "message_id", "device_id", "session_id",
         "turn_id", "stream_id", "epoch", "payload"}};
    for (std::size_t index = 0U; index < fields.size(); ++index) {
      if (key == fields[index]) {
        return static_cast<int>(index);
      }
    }
    return -1;
  }

  static bool ParseType(const std::string& type,
                        ServerControlType* const output) {
    if (type == "hello.ack") {
      *output = ServerControlType::kHelloAck;
    } else if (type == "response.start") {
      *output = ServerControlType::kResponseStart;
    } else if (type == "response.text_delta") {
      *output = ServerControlType::kResponseTextDelta;
    } else if (type == "response.audio_start") {
      *output = ServerControlType::kResponseAudioStart;
    } else if (type == "response.cancelled") {
      *output = ServerControlType::kResponseCancelled;
    } else if (type == "response.done") {
      *output = ServerControlType::kResponseDone;
    } else if (type == "error") {
      *output = ServerControlType::kError;
    } else {
      return false;
    }
    return true;
  }

  static int PayloadFieldIndex(const ServerControlType type,
                               const std::string& key) {
    switch (type) {
      case ServerControlType::kHelloAck:
        if (key == "input_sample_rate_hz") {
          return 0;
        }
        if (key == "output_sample_rate_hz") {
          return 1;
        }
        return key == "input_frame_ms" ? 2 : -1;
      case ServerControlType::kResponseStart:
      case ServerControlType::kResponseDone:
        return key == "response_id" ? 0 : -1;
      case ServerControlType::kResponseCancelled:
        return key == "reason" ? 0 : -1;
      case ServerControlType::kResponseTextDelta:
        if (key == "response_id") {
          return 0;
        }
        return key == "text" ? 1 : -1;
      case ServerControlType::kResponseAudioStart:
        if (key == "response_id") {
          return 0;
        }
        return key == "sample_rate_hz" ? 1 : -1;
      case ServerControlType::kError:
        if (key == "code") {
          return 0;
        }
        return key == "message" ? 1 : -1;
    }
    return -1;
  }

  bool ParsePayloadValue(const ServerControlType type, const int field,
                         ServerControlMessage* const message) {
    switch (type) {
      case ServerControlType::kHelloAck:
        if (field == 0) {
          return ParseUint32(&message->input_sample_rate_hz, 2U);
        }
        if (field == 1) {
          return ParseUint32(&message->output_sample_rate_hz, 2U);
        }
        return ParseUint32(&message->input_frame_ms, 2U);
      case ServerControlType::kResponseStart:
      case ServerControlType::kResponseDone:
        return EnterNode(2U) &&
               ParseString(&message->response_id, kMaximumResponseIdBytes);
      case ServerControlType::kResponseCancelled:
        return EnterNode(2U) &&
               ParseString(&message->cancellation_reason,
                           kMaximumCancellationReasonBytes);
      case ServerControlType::kResponseTextDelta:
        if (field == 0) {
          return EnterNode(2U) &&
                 ParseString(&message->response_id,
                             kMaximumResponseIdBytes);
        }
        return EnterNode(2U) &&
               ParseString(&message->text, kMaximumTextDeltaBytes);
      case ServerControlType::kResponseAudioStart:
        if (field == 0) {
          return EnterNode(2U) &&
                 ParseString(&message->response_id,
                             kMaximumResponseIdBytes);
        }
        return ParseUint32(&message->sample_rate_hz, 2U);
      case ServerControlType::kError:
        if (field == 0) {
          return EnterNode(2U) &&
                 ParseString(&message->error_code,
                             kMaximumErrorCodeBytes);
        }
        return EnterNode(2U) &&
               ParseString(&message->error_message,
                           kMaximumErrorMessageBytes);
    }
    return Fail(StatusCode::kInternal, "invalid server control type");
  }

  bool ValidatePayloadStrings(const ServerControlType type,
                              const ServerControlMessage& message) {
    if (type == ServerControlType::kResponseStart ||
        type == ServerControlType::kResponseTextDelta ||
        type == ServerControlType::kResponseAudioStart ||
        type == ServerControlType::kResponseDone) {
      if (!IsVisibleAscii(message.response_id, kMaximumResponseIdBytes)) {
        return Fail(StatusCode::kInvalidArgument,
                    "response_id must contain bounded visible ASCII");
      }
    }
    if (type == ServerControlType::kResponseTextDelta &&
        message.text.empty()) {
      return Fail(StatusCode::kInvalidArgument,
                  "response text delta must not be empty");
    }
    if (type == ServerControlType::kResponseCancelled &&
        !IsVisibleAscii(message.cancellation_reason,
                        kMaximumCancellationReasonBytes)) {
      return Fail(StatusCode::kInvalidArgument,
                  "cancellation reason must contain bounded visible ASCII");
    }
    if (type == ServerControlType::kError) {
      if (!IsVisibleAscii(message.error_code, kMaximumErrorCodeBytes)) {
        return Fail(StatusCode::kInvalidArgument,
                    "error code must contain bounded visible ASCII");
      }
      if (message.error_message.empty()) {
        return Fail(StatusCode::kInvalidArgument,
                    "error message must not be empty");
      }
    }
    return true;
  }

  bool EnterNode(const std::size_t depth) {
    if (depth > kMaximumJsonDepth) {
      return Fail(StatusCode::kResourceExhausted,
                  "JSON exceeds the maximum depth");
    }
    ++nodes_;
    if (nodes_ > kMaximumJsonNodes) {
      return Fail(StatusCode::kResourceExhausted,
                  "JSON exceeds the maximum node count");
    }
    return true;
  }

  bool Fail(const StatusCode code, std::string message) {
    if (status_.ok()) {
      status_ = Status::Error(code, std::move(message));
    }
    return false;
  }

  void Space() {
    while (position_ < input_.size()) {
      const char current = input_[position_];
      if (current != ' ' && current != '\t' && current != '\r' &&
          current != '\n') {
        break;
      }
      ++position_;
    }
  }

  bool Try(const char expected) {
    Space();
    if (position_ >= input_.size() || input_[position_] != expected) {
      return false;
    }
    ++position_;
    return true;
  }

  bool Expect(const char expected, const char* const message) {
    return Try(expected) ? true : Fail(StatusCode::kInvalidArgument, message);
  }

  bool ParseString(std::string* const output,
                   const std::size_t maximum_bytes) {
    Space();
    if (position_ >= input_.size() || input_[position_] != '"') {
      return Fail(StatusCode::kInvalidArgument, "expected a JSON string");
    }
    ++position_;
    if (output != nullptr) {
      output->clear();
    }
    std::size_t decoded_bytes = 0U;
    while (position_ < input_.size()) {
      const auto current =
          static_cast<unsigned char>(input_[position_++]);
      if (current == static_cast<unsigned char>('"')) {
        return true;
      }
      if (current < 0x20U) {
        return Fail(StatusCode::kInvalidArgument,
                    "JSON string contains a control byte");
      }
      if (current != static_cast<unsigned char>('\\')) {
        if (!AppendByte(static_cast<char>(current), maximum_bytes,
                        &decoded_bytes, output)) {
          return false;
        }
        continue;
      }
      if (position_ >= input_.size()) {
        return Fail(StatusCode::kInvalidArgument, "truncated JSON escape");
      }
      const char escaped = input_[position_++];
      constexpr char simple[] = "\"\\/bfnrt";
      constexpr char decoded[] = "\"\\/\b\f\n\r\t";
      const char* const found =
          std::find(simple, simple + sizeof(simple) - 1U, escaped);
      if (found != simple + sizeof(simple) - 1U) {
        if (!AppendByte(decoded[static_cast<std::size_t>(found - simple)],
                        maximum_bytes, &decoded_bytes, output)) {
          return false;
        }
        continue;
      }
      if (escaped != 'u') {
        return Fail(StatusCode::kInvalidArgument, "invalid JSON escape");
      }
      std::uint32_t code_point = 0U;
      if (!ParseHex4(&code_point)) {
        return false;
      }
      if (code_point >= 0xD800U && code_point <= 0xDBFFU) {
        if (position_ + 2U > input_.size() || input_[position_] != '\\' ||
            input_[position_ + 1U] != 'u') {
          return Fail(StatusCode::kInvalidArgument,
                      "incomplete JSON surrogate pair");
        }
        position_ += 2U;
        std::uint32_t low = 0U;
        if (!ParseHex4(&low)) {
          return false;
        }
        if (low < 0xDC00U || low > 0xDFFFU) {
          return Fail(StatusCode::kInvalidArgument,
                      "invalid JSON surrogate pair");
        }
        code_point = 0x10000U + ((code_point - 0xD800U) << 10U) +
                     (low - 0xDC00U);
      } else if (code_point >= 0xDC00U && code_point <= 0xDFFFU) {
        return Fail(StatusCode::kInvalidArgument,
                    "unpaired JSON low surrogate");
      }
      if (!AppendCodePoint(code_point, maximum_bytes, &decoded_bytes,
                           output)) {
        return false;
      }
    }
    return Fail(StatusCode::kInvalidArgument, "unterminated JSON string");
  }

  bool AppendByte(const char byte, const std::size_t maximum_bytes,
                  std::size_t* const decoded_bytes,
                  std::string* const output) {
    if (*decoded_bytes >= maximum_bytes) {
      return Fail(StatusCode::kResourceExhausted,
                  "JSON string exceeds its byte limit");
    }
    ++(*decoded_bytes);
    if (output != nullptr) {
      output->push_back(byte);
    }
    return true;
  }

  bool AppendCodePoint(const std::uint32_t code_point,
                       const std::size_t maximum_bytes,
                       std::size_t* const decoded_bytes,
                       std::string* const output) {
    std::array<char, 4U> bytes{};
    std::size_t byte_count = 0U;
    if (code_point <= 0x7FU) {
      bytes[0] = static_cast<char>(code_point);
      byte_count = 1U;
    } else if (code_point <= 0x7FFU) {
      bytes[0] = static_cast<char>(0xC0U | (code_point >> 6U));
      bytes[1] = static_cast<char>(0x80U | (code_point & 0x3FU));
      byte_count = 2U;
    } else if (code_point <= 0xFFFFU) {
      bytes[0] = static_cast<char>(0xE0U | (code_point >> 12U));
      bytes[1] = static_cast<char>(0x80U | ((code_point >> 6U) & 0x3FU));
      bytes[2] = static_cast<char>(0x80U | (code_point & 0x3FU));
      byte_count = 3U;
    } else {
      bytes[0] = static_cast<char>(0xF0U | (code_point >> 18U));
      bytes[1] = static_cast<char>(0x80U | ((code_point >> 12U) & 0x3FU));
      bytes[2] = static_cast<char>(0x80U | ((code_point >> 6U) & 0x3FU));
      bytes[3] = static_cast<char>(0x80U | (code_point & 0x3FU));
      byte_count = 4U;
    }
    if (byte_count > maximum_bytes -
                         std::min(maximum_bytes, *decoded_bytes)) {
      return Fail(StatusCode::kResourceExhausted,
                  "JSON string exceeds its byte limit");
    }
    *decoded_bytes += byte_count;
    if (output != nullptr) {
      output->append(bytes.data(), byte_count);
    }
    return true;
  }

  bool ParseHex4(std::uint32_t* const output) {
    if (position_ + 4U > input_.size()) {
      return Fail(StatusCode::kInvalidArgument,
                  "truncated JSON unicode escape");
    }
    std::uint32_t value = 0U;
    for (std::size_t count = 0U; count < 4U; ++count) {
      const char current = input_[position_++];
      std::uint32_t digit = 0U;
      if (current >= '0' && current <= '9') {
        digit = static_cast<std::uint32_t>(current - '0');
      } else if (current >= 'a' && current <= 'f') {
        digit = static_cast<std::uint32_t>(current - 'a') + 10U;
      } else if (current >= 'A' && current <= 'F') {
        digit = static_cast<std::uint32_t>(current - 'A') + 10U;
      } else {
        return Fail(StatusCode::kInvalidArgument,
                    "JSON unicode escape is not hexadecimal");
      }
      value = (value << 4U) | digit;
    }
    *output = value;
    return true;
  }

  bool ParseUint32(std::uint32_t* const output, const std::size_t depth) {
    if (!EnterNode(depth)) {
      return false;
    }
    Space();
    if (position_ >= input_.size() || input_[position_] < '0' ||
        input_[position_] > '9') {
      return Fail(StatusCode::kInvalidArgument,
                  "expected a JSON uint32");
    }
    if (input_[position_] == '0' && position_ + 1U < input_.size() &&
        input_[position_ + 1U] >= '0' && input_[position_ + 1U] <= '9') {
      return Fail(StatusCode::kInvalidArgument,
                  "JSON uint32 has a leading zero");
    }
    std::uint32_t value = 0U;
    constexpr std::uint32_t maximum =
        std::numeric_limits<std::uint32_t>::max();
    while (position_ < input_.size() && input_[position_] >= '0' &&
           input_[position_] <= '9') {
      const auto digit =
          static_cast<std::uint32_t>(input_[position_] - '0');
      if (value > (maximum - digit) / 10U) {
        return Fail(StatusCode::kInvalidArgument,
                    "JSON uint32 is outside range");
      }
      value = value * 10U + digit;
      ++position_;
    }
    if (position_ < input_.size() &&
        (input_[position_] == '.' || input_[position_] == 'e' ||
         input_[position_] == 'E')) {
      return Fail(StatusCode::kInvalidArgument,
                  "JSON uint32 is not an integer");
    }
    *output = value;
    return true;
  }

  bool SkipValue(const std::size_t depth) {
    if (!EnterNode(depth)) {
      return false;
    }
    Space();
    if (position_ >= input_.size()) {
      return Fail(StatusCode::kInvalidArgument, "missing JSON value");
    }
    if (input_[position_] == '"') {
      return ParseString(nullptr, kMaximumControlFrameBytes);
    }
    if (input_[position_] == '{') {
      return SkipObject(depth);
    }
    if (input_[position_] == '[') {
      return SkipArray(depth);
    }
    if (input_.compare(position_, 4U, "true") == 0) {
      position_ += 4U;
      return true;
    }
    if (input_.compare(position_, 5U, "false") == 0) {
      position_ += 5U;
      return true;
    }
    if (input_.compare(position_, 4U, "null") == 0) {
      position_ += 4U;
      return true;
    }
    return SkipNumber();
  }

  bool SkipObject(const std::size_t depth) {
    if (!Expect('{', "invalid JSON object")) {
      return false;
    }
    std::vector<std::string> keys;
    if (Try('}')) {
      return true;
    }
    for (;;) {
      std::string key;
      if (!ParseString(&key, kMaximumJsonKeyBytes)) {
        return false;
      }
      if (std::find(keys.begin(), keys.end(), key) != keys.end()) {
        return Fail(StatusCode::kInvalidArgument,
                    "JSON object contains a duplicate key");
      }
      keys.push_back(std::move(key));
      if (!Expect(':', "JSON object field needs a colon") ||
          !SkipValue(depth + 1U)) {
        return false;
      }
      if (Try('}')) {
        return true;
      }
      if (!Expect(',', "JSON object fields must be comma-separated")) {
        return false;
      }
    }
  }

  bool SkipArray(const std::size_t depth) {
    if (!Expect('[', "invalid JSON array")) {
      return false;
    }
    if (Try(']')) {
      return true;
    }
    for (;;) {
      if (!SkipValue(depth + 1U)) {
        return false;
      }
      if (Try(']')) {
        return true;
      }
      if (!Expect(',', "JSON array values must be comma-separated")) {
        return false;
      }
    }
  }

  bool SkipNumber() {
    if (position_ < input_.size() && input_[position_] == '-') {
      ++position_;
    }
    if (position_ >= input_.size()) {
      return Fail(StatusCode::kInvalidArgument, "truncated JSON number");
    }
    if (input_[position_] == '0') {
      ++position_;
      if (position_ < input_.size() && input_[position_] >= '0' &&
          input_[position_] <= '9') {
        return Fail(StatusCode::kInvalidArgument,
                    "JSON number has a leading zero");
      }
    } else if (input_[position_] >= '1' && input_[position_] <= '9') {
      while (position_ < input_.size() && input_[position_] >= '0' &&
             input_[position_] <= '9') {
        ++position_;
      }
    } else {
      return Fail(StatusCode::kInvalidArgument, "invalid JSON value");
    }
    if (position_ < input_.size() && input_[position_] == '.') {
      ++position_;
      if (position_ >= input_.size() || input_[position_] < '0' ||
          input_[position_] > '9') {
        return Fail(StatusCode::kInvalidArgument, "invalid JSON fraction");
      }
      while (position_ < input_.size() && input_[position_] >= '0' &&
             input_[position_] <= '9') {
        ++position_;
      }
    }
    if (position_ < input_.size() &&
        (input_[position_] == 'e' || input_[position_] == 'E')) {
      ++position_;
      if (position_ < input_.size() &&
          (input_[position_] == '+' || input_[position_] == '-')) {
        ++position_;
      }
      if (position_ >= input_.size() || input_[position_] < '0' ||
          input_[position_] > '9') {
        return Fail(StatusCode::kInvalidArgument, "invalid JSON exponent");
      }
      while (position_ < input_.size() && input_[position_] >= '0' &&
             input_[position_] <= '9') {
        ++position_;
      }
    }
    return true;
  }

  std::string_view input_;
  std::size_t position_{0U};
  std::size_t nodes_{0U};
  Status status_{};
};

}  // namespace

Status DecodeServerControl(const std::string_view control_json,
                           const std::string_view expected_device_id,
                           ServerControlMessage* const output) {
  if (output == nullptr) {
    return Status::Error(StatusCode::kInvalidArgument,
                         "server control output is null");
  }
  if (control_json.empty() ||
      control_json.size() > kMaximumControlFrameBytes) {
    return Status::Error(StatusCode::kResourceExhausted,
                         "control frame size is outside allowed bounds");
  }
  if (!IsValidUtf8(control_json)) {
    return Status::Error(StatusCode::kInvalidArgument,
                         "control frame is not valid UTF-8");
  }

  ServerControlMessage decoded{};
  std::string_view payload_json;
  JsonCursor envelope_parser(control_json);
  if (!envelope_parser.ParseTopLevel(expected_device_id, &decoded,
                                     &payload_json)) {
    const Status status = envelope_parser.status();
    if (!status.ok()) {
      return status;
    }
    return Status::Error(StatusCode::kNotSupported,
                         "unsupported server control type");
  }

  JsonCursor payload_parser(payload_json);
  if (!payload_parser.ParsePayload(decoded.type, &decoded)) {
    return payload_parser.status();
  }
  *output = std::move(decoded);
  return Status::Ok();
}

}  // namespace boompi::protocol
