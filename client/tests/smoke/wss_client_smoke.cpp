#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "boompi/network/wss_client.h"
namespace {

constexpr char kDeviceId[] = "00112233-4455-6677-8899-aabbccddeeff";
constexpr std::uint32_t kTurnId = 11U;
constexpr std::uint32_t kUplinkStreamId = 12U;
constexpr std::size_t kMaximumJsonDepth = 16U;
constexpr std::size_t kMaximumJsonNodes = 256U;
constexpr std::size_t kMaximumSmokeMessages = 16U;
constexpr std::size_t kMaximumDownlinkBytes = 4096U;
struct ControlEnvelope final {
  std::uint32_t version{0U}, session_id{0U}, turn_id{0U}, stream_id{0U},
      epoch{0U};
  std::string type, message_id, device_id;
};
class ControlParser final {
 public:
  explicit ControlParser(const std::string& input) : input_(input) {}

  bool Parse(ControlEnvelope* const output) {
    if (output == nullptr || !Expect('{', "control must be a JSON object")) {
      return false;
    }
    ControlEnvelope decoded{};
    std::array<bool, 9> seen{};
    if (Try('}')) {
      return Fail("control is missing required fields");
    }
    for (;;) {
      std::string key;
      if (!String(&key) || !Expect(':', "control field needs a colon")) {
        return false;
      }
      const int field = FieldIndex(key);
      if (field < 0) {
        return Fail("control contains an unknown top-level field");
      }
      if (seen[static_cast<std::size_t>(field)]) {
        return Fail("control contains a duplicate top-level field");
      }
      seen[static_cast<std::size_t>(field)] = true;
      switch (field) {
        case 0: if (!Uint32(&decoded.version)) return false; break;
        case 1: if (!String(&decoded.type)) return false; break;
        case 2: if (!String(&decoded.message_id)) return false; break;
        case 3: if (!String(&decoded.device_id)) return false; break;
        case 4: if (!Uint32(&decoded.session_id)) return false; break;
        case 5: if (!Uint32(&decoded.turn_id)) return false; break;
        case 6: if (!Uint32(&decoded.stream_id)) return false; break;
        case 7: if (!Uint32(&decoded.epoch)) return false; break;
        case 8:
          Space();
          if (position_ >= input_.size() || input_[position_] != '{') {
            return Fail("control payload must be an object");
          }
          if (!SkipValue(0U)) return false;
          break;
        default:
          return Fail("invalid control field");
      }
      if (Try('}')) break;
      if (!Expect(',', "control fields must be comma-separated")) return false;
    }
    Space();
    if (position_ != input_.size()) return Fail("control has trailing data");
    if (std::find(seen.begin(), seen.end(), false) != seen.end()) {
      return Fail("control is missing a required field");
    }
    const bool type_valid = std::all_of(decoded.type.begin(), decoded.type.end(),
        [](const char value) { return value >= 0x21 && value <= 0x7e; });
    if (decoded.version != 1U || decoded.type.empty() || !type_valid ||
        decoded.type.size() > 64U || decoded.message_id.empty() ||
        decoded.message_id.size() > 64U || decoded.device_id != kDeviceId) {
      return Fail("control metadata is outside protocol bounds");
    }
    *output = decoded;
    return true;
  }
  const std::string& error() const noexcept { return error_; }
 private:
  static int FieldIndex(const std::string& key) {
    constexpr std::array<const char*, 9> fields{{"version", "type", "message_id",
        "device_id", "session_id", "turn_id", "stream_id", "epoch", "payload"}};
    for (std::size_t index = 0U; index < fields.size(); ++index) {
      if (key == fields[index]) return static_cast<int>(index);
    }
    return -1;
  }
  bool Fail(const char* const message) {
    if (error_.empty()) error_ = message;
    return false;
  }
  void Space() {
    while (position_ < input_.size()) {
      const char value = input_[position_];
      if (value != ' ' && value != '\t' && value != '\r' && value != '\n') {
        break;
      }
      ++position_;
    }
  }
  bool Try(const char expected) {
    Space();
    if (position_ >= input_.size() || input_[position_] != expected) return false;
    ++position_;
    return true;
  }
  bool Expect(const char expected, const char* const message) {
    return Try(expected) ? true : Fail(message);
  }
  bool String(std::string* const output) {
    Space();
    if (position_ >= input_.size() || input_[position_++] != '"') {
      return Fail("expected a JSON string");
    }
    if (output != nullptr) output->clear();
    while (position_ < input_.size()) {
      const auto value = static_cast<unsigned char>(input_[position_++]);
      if (value == '"') return true;
      if (value < 0x20U) return Fail("JSON string has a control byte");
      if (value != '\\') {
        if (output != nullptr) output->push_back(static_cast<char>(value));
        continue;
      }
      if (position_ >= input_.size()) return Fail("truncated JSON escape");
      const char escaped = input_[position_++];
      const char simple[] = "\"\\/bfnrt";
      const char decoded[] = "\"\\/\b\f\n\r\t";
      const char* const found =
          std::find(simple, simple + sizeof(simple) - 1U, escaped);
      if (found != simple + sizeof(simple) - 1U) {
        if (output != nullptr)
          output->push_back(decoded[static_cast<std::size_t>(found - simple)]);
        continue;
      }
      if (escaped != 'u') return Fail("invalid JSON escape");
      std::uint32_t code_point = 0U;
      if (!Hex4(&code_point)) return false;
      if (code_point >= 0xD800U && code_point <= 0xDBFFU) {
        if (position_ + 2U > input_.size() || input_[position_] != '\\' ||
            input_[position_ + 1U] != 'u') {
          return Fail("incomplete JSON surrogate pair");
        }
        position_ += 2U;
        std::uint32_t low = 0U;
        if (!Hex4(&low)) return false;
        if (low < 0xDC00U || low > 0xDFFFU) {
          return Fail("invalid JSON surrogate pair");
        }
        code_point = 0x10000U + ((code_point - 0xD800U) << 10U) +
                     (low - 0xDC00U);
      } else if (code_point >= 0xDC00U && code_point <= 0xDFFFU) {
        return Fail("unpaired JSON low surrogate");
      }
      if (output != nullptr) {
        if (code_point <= 0x7FU) output->push_back(static_cast<char>(code_point));
        else output->append("\xEF\xBF\xBD");
      }
    }
    return Fail("unterminated JSON string");
  }
  bool Hex4(std::uint32_t* const output) {
    if (position_ + 4U > input_.size()) return Fail("truncated unicode escape");
    std::uint32_t value = 0U;
    for (std::size_t count = 0U; count < 4U; ++count) {
      const char current = input_[position_++];
      std::uint32_t digit = 0U;
      if (current >= '0' && current <= '9') digit = current - '0';
      else if (current >= 'a' && current <= 'f') digit = current - 'a' + 10;
      else if (current >= 'A' && current <= 'F') digit = current - 'A' + 10;
      else return Fail("unicode escape is not hexadecimal");
      value = (value << 4U) | digit;
    }
    *output = value;
    return true;
  }
  bool Uint32(std::uint32_t* const output) {
    Space();
    if (position_ >= input_.size() || input_[position_] < '0' ||
        input_[position_] > '9') return Fail("expected a JSON uint32");
    if (input_[position_] == '0' && position_ + 1U < input_.size() &&
        input_[position_ + 1U] >= '0' && input_[position_ + 1U] <= '9') {
      return Fail("JSON uint32 has a leading zero");
    }
    std::uint64_t value = 0U;
    while (position_ < input_.size() && input_[position_] >= '0' &&
           input_[position_] <= '9') {
      value = value * 10U + static_cast<std::uint64_t>(input_[position_++] - '0');
      if (value > std::numeric_limits<std::uint32_t>::max())
        return Fail("JSON uint32 is outside range");
    }
    if (position_ < input_.size() &&
        (input_[position_] == '.' || input_[position_] == 'e' ||
         input_[position_] == 'E')) return Fail("JSON uint32 is not an integer");
    *output = static_cast<std::uint32_t>(value);
    return true;
  }
  bool SkipValue(const std::size_t depth) {
    if (depth > kMaximumJsonDepth || ++nodes_ > kMaximumJsonNodes) {
      return Fail("JSON payload exceeds its structural bound");
    }
    Space();
    if (position_ >= input_.size()) return Fail("missing JSON value");
    if (input_[position_] == '"') return String(nullptr);
    if (input_[position_] == '{') return SkipCollection('{', '}', depth);
    if (input_[position_] == '[') return SkipCollection('[', ']', depth);
    if (input_.compare(position_, 4U, "true") == 0) { position_ += 4U; return true; }
    if (input_.compare(position_, 5U, "false") == 0) { position_ += 5U; return true; }
    if (input_.compare(position_, 4U, "null") == 0) { position_ += 4U; return true; }
    return SkipNumber();
  }
  bool SkipCollection(const char opening, const char closing,
                      const std::size_t depth) {
    if (!Expect(opening, "invalid JSON collection")) return false;
    if (Try(closing)) return true;
    for (;;) {
      if (opening == '{') {
        if (!String(nullptr) || !Expect(':', "object field needs a colon")) {
          return false;
        }
      }
      if (!SkipValue(depth + 1U)) return false;
      if (Try(closing)) return true;
      if (!Expect(',', "collection values must be comma-separated")) return false;
    }
  }
  bool SkipNumber() {
    if (position_ < input_.size() && input_[position_] == '-') ++position_;
    if (position_ >= input_.size()) return Fail("truncated JSON number");
    if (input_[position_] == '0') ++position_;
    else if (input_[position_] >= '1' && input_[position_] <= '9') {
      while (position_ < input_.size() && input_[position_] >= '0' &&
             input_[position_] <= '9') ++position_;
    } else return Fail("invalid JSON value");
    if (position_ < input_.size() && input_[position_] == '.') {
      ++position_;
      if (position_ >= input_.size() || input_[position_] < '0' ||
          input_[position_] > '9') return Fail("invalid JSON fraction");
      while (position_ < input_.size() && input_[position_] >= '0' &&
             input_[position_] <= '9') ++position_;
    }
    if (position_ < input_.size() &&
        (input_[position_] == 'e' || input_[position_] == 'E')) {
      ++position_;
      if (position_ < input_.size() &&
          (input_[position_] == '+' || input_[position_] == '-')) ++position_;
      if (position_ >= input_.size() || input_[position_] < '0' ||
          input_[position_] > '9') return Fail("invalid JSON exponent");
      while (position_ < input_.size() && input_[position_] >= '0' &&
             input_[position_] <= '9') ++position_;
    }
    return true;
  }

  const std::string& input_;
  std::size_t position_{0U};
  std::size_t nodes_{0U};
  std::string error_;
};
int Fail(const char* const operation, const boompi::Status& status) {
  std::cerr << "boompi-wss-client-smoke: " << operation
            << " failed: " << status.message() << '\n';
  return EXIT_FAILURE;
}
int FailMessage(const std::string& message) {
  std::cerr << "boompi-wss-client-smoke: " << message << '\n';
  return EXIT_FAILURE;
}
void JsonString(const std::string& value, std::string* const output) {
  output->push_back('"');
  for (const char current : value) {
    if (current == '"') output->append("\\\"");
    else if (current == '\\') output->append("\\\\");
    else output->push_back(current);
  }
  output->push_back('"');
}
std::string Control(const std::string& type, const char* const message_id,
                    const std::uint32_t session_id,
                    const std::uint32_t turn_id,
                    const std::uint32_t stream_id, const std::uint32_t epoch,
                    const std::string& payload) {
  std::string value("{\"version\":1,\"type\":\"");
  value.append(type).append("\",\"message_id\":\"").append(message_id);
  value.append("\",\"device_id\":\"").append(kDeviceId);
  value.append("\",\"session_id\":").append(std::to_string(session_id));
  value.append(",\"turn_id\":").append(std::to_string(turn_id));
  value.append(",\"stream_id\":").append(std::to_string(stream_id));
  value.append(",\"epoch\":").append(std::to_string(epoch));
  value.append(",\"payload\":").append(payload).push_back('}');
  return value;
}
bool Decode(const boompi::network::WssInboundMessage& message,
            ControlEnvelope* const output, std::string* const error) {
  if (message.type != boompi::network::WssInboundMessageType::kControl) {
    *error = "expected a control frame";
    return false;
  }
  ControlParser parser(message.control_json);
  if (!parser.Parse(output)) {
    *error = parser.error();
    return false;
  }
  return true;
}
}  // namespace
int main(const int argc, char* argv[]) {
  if (argc != 4) {
    std::cerr << "usage: boompi-wss-client-smoke <server-ip> <port> "
                 "<spki-sha256-base64>\n";
    return EXIT_FAILURE;
  }
  const char* const token_value = std::getenv("BOOMPI_DEVICE_TOKEN");
  const std::string token = token_value == nullptr ? "" : token_value;
  const bool token_valid = token.size() >= 32U && token.size() <= 256U &&
      std::all_of(token.begin(), token.end(), [](const char current) {
        const auto byte = static_cast<unsigned char>(current);
        return byte > 0x20U && byte != 0x7FU;
      });
  if (!token_valid) {
    return FailMessage("BOOMPI_DEVICE_TOKEN is missing or invalid");
  }
  char* port_end = nullptr;
  const unsigned long port = std::strtoul(argv[2], &port_end, 10);
  if (port_end == argv[2] || *port_end != '\0' || port == 0UL ||
      port > 65535UL) return FailMessage("invalid port");
  boompi::network::WssClientConfig config{
      argv[1], static_cast<std::uint16_t>(port), "boompi.test", "", argv[3],
      5000U, 5000U, 1000U};
  auto unsafe_config = config;
  unsafe_config.server_name = "boompi.test\r\nHost: wrong.example";
  if (boompi::network::ValidateWssClientConfig(unsafe_config).ok()) {
    return FailMessage("unsafe TLS server name was accepted");
  }
  boompi::network::WssClient client(std::move(config));
  auto status = client.Connect();
  if (!status.ok()) return Fail("connect", status);
  std::string hello_payload("{\"device_token\":");
  JsonString(token, &hello_payload);
  hello_payload.push_back('}');
  status = client.SendControl(
      Control("hello", "smoke-1", 0U, 0U, 0U, 0U, hello_payload));
  if (!status.ok()) return Fail("send hello", status);

  boompi::network::WssInboundMessage inbound{};
  status = client.Receive(&inbound);
  if (!status.ok()) return Fail("receive hello.ack", status);
  ControlEnvelope hello_ack{};
  std::string error;
  if (!Decode(inbound, &hello_ack, &error)) {
    return FailMessage("decode hello.ack: " + error);
  }
  if (hello_ack.type != "hello.ack" || hello_ack.session_id == 0U ||
      hello_ack.turn_id != 0U || hello_ack.stream_id != 0U ||
      hello_ack.epoch == 0U) return FailMessage("invalid hello.ack identifiers");
  const std::uint32_t session_id = hello_ack.session_id;
  const std::uint32_t epoch = hello_ack.epoch;
  status = client.SendControl(Control(
      "turn.start", "smoke-2", session_id, kTurnId, kUplinkStreamId, epoch,
      "{\"sample_rate_hz\":16000}"));
  if (!status.ok()) return Fail("send turn.start", status);
  std::array<std::uint8_t, 640> pcm{};
  pcm[0] = 7U;
  pcm[1] = 8U;
  boompi::protocol::AudioPacketHeader header{};
  header.kind = boompi::protocol::AudioPacketKind::kUplink;
  header.flags = 0x0003U;
  header.sample_rate_hz = 16000U;
  header.payload_length_bytes = static_cast<std::uint32_t>(pcm.size());
  header.sequence = 0U;
  header.monotonic_timestamp_us = 1U;
  header.epoch = epoch;
  header.device_uuid = {{0x00U, 0x11U, 0x22U, 0x33U, 0x44U, 0x55U,
                         0x66U, 0x77U, 0x88U, 0x99U, 0xAAU, 0xBBU,
                         0xCCU, 0xDDU, 0xEEU, 0xFFU}};
  header.session_id = session_id;
  header.turn_id = kTurnId;
  header.stream_id = kUplinkStreamId;
  status = client.SendPcm(header, pcm.data(), pcm.size());
  if (!status.ok()) return Fail("send PCM", status);
  status = client.SendControl(Control(
      "turn.commit", "smoke-3", session_id, kTurnId, kUplinkStreamId, epoch,
      "{}"));
  if (!status.ok()) return Fail("send turn.commit", status);
  bool got_start = false;
  bool got_text = false;
  bool got_audio_start = false;
  bool got_done = false;
  std::uint32_t downlink_stream_id = 0U;
  std::uint32_t output_sequence = 0U;
  std::vector<std::uint8_t> output_pcm;
  for (std::size_t count = 0U;
       count < kMaximumSmokeMessages && !got_done; ++count) {
    inbound = {};
    status = client.Receive(&inbound);
    if (!status.ok()) return Fail("receive response", status);
    if (inbound.type == boompi::network::WssInboundMessageType::kPcm24k) {
      const auto& current = inbound.pcm_header;
      if (!got_audio_start || current.device_uuid != header.device_uuid ||
          current.session_id != session_id || current.turn_id != kTurnId ||
          current.stream_id != downlink_stream_id || current.epoch != epoch ||
          current.sequence != output_sequence ||
          current.payload_length_bytes != inbound.pcm_payload.size()) {
        return FailMessage("downlink PCM identifiers are invalid");
      }
      if (inbound.pcm_payload.size() >
          kMaximumDownlinkBytes - output_pcm.size()) {
        return FailMessage("downlink PCM exceeds smoke bound");
      }
      output_pcm.insert(output_pcm.end(), inbound.pcm_payload.begin(),
                        inbound.pcm_payload.end());
      ++output_sequence;
      continue;
    }
    ControlEnvelope response{};
    error.clear();
    if (!Decode(inbound, &response, &error)) {
      return FailMessage("decode response: " + error);
    }
    if (response.session_id != session_id || response.turn_id != kTurnId ||
        response.epoch != epoch || response.stream_id == 0U ||
        (downlink_stream_id != 0U &&
         response.stream_id != downlink_stream_id)) {
      return FailMessage("response identifiers are invalid");
    }
    if (response.type == "response.start" && !got_start) {
      got_start = true;
      downlink_stream_id = response.stream_id;
    } else if (response.type == "response.text_delta" && got_start) {
      got_text = true;
    } else if (response.type == "response.audio_start" && got_start &&
               !got_audio_start) {
      got_audio_start = true;
    } else if (response.type == "response.done" && got_start) {
      got_done = true;
    } else {
      return FailMessage("unexpected response control sequence");
    }
  }
  const std::vector<std::uint8_t> expected_pcm{1U, 2U, 3U, 4U};
  if (!got_start || !got_text || !got_audio_start || !got_done ||
      output_pcm != expected_pcm) {
    return FailMessage("deterministic response path is incomplete");
  }

  std::atomic<bool> receive_entered{false};
  std::atomic<bool> receive_done{false};
  boompi::Status blocked_receive = boompi::Status::Error(
      boompi::StatusCode::kInternal, "cancel smoke did not run");
  std::thread receiver(
      [&client, &blocked_receive, &receive_entered, &receive_done]() {
    receive_entered.store(true, std::memory_order_release);
    boompi::network::WssInboundMessage ignored{};
    blocked_receive = client.Receive(&ignored);
    receive_done.store(true, std::memory_order_release);
  });
  const auto enter_deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(100);
  while (!receive_entered.load(std::memory_order_acquire) &&
         std::chrono::steady_clock::now() < enter_deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  if (!receive_entered.load(std::memory_order_acquire)) {
    std::cerr << "boompi-wss-client-smoke: receive thread did not start\n";
    std::_Exit(EXIT_FAILURE);
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(25));
  if (receive_done.load(std::memory_order_acquire)) {
    receiver.join();
    client.Close();
    return FailMessage("receive did not block before cancellation");
  }
  const auto stop_started = std::chrono::steady_clock::now();
  client.RequestStop();
  const auto stop_deadline =
      stop_started + std::chrono::milliseconds(500);
  while (!receive_done.load(std::memory_order_acquire) &&
         std::chrono::steady_clock::now() < stop_deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  if (!receive_done.load(std::memory_order_acquire)) {
    std::cerr << "boompi-wss-client-smoke: RequestStop did not unblock receive\n";
    std::_Exit(EXIT_FAILURE);
  }
  receiver.join();
  client.Close();
  if (blocked_receive.code() != boompi::StatusCode::kFailedPrecondition ||
      blocked_receive.message() != "network operation cancelled") {
    return FailMessage("RequestStop returned the wrong cancellation status");
  }
  std::cout << "WSS_SMOKE_OK\n";
  return EXIT_SUCCESS;
}
