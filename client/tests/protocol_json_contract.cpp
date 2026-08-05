#include "../src/network/voice_transport.cpp"

#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace {

using boompi::network::ParseEnvelope;
using boompi::network::RequireUint32;

bool AcceptEnvelope(const std::string& json) {
  try {
    (void)ParseEnvelope(json);
    return true;
  } catch (...) {
    return false;
  }
}

bool AcceptHelloAck(const std::string& json) {
  try {
    const auto envelope = ParseEnvelope(json);
    if (envelope.type != "hello.ack") return false;
    return RequireUint32(envelope.payload, "input_sample_rate_hz") == 16000U &&
           RequireUint32(envelope.payload, "output_sample_rate_hz") == 24000U &&
           RequireUint32(envelope.payload, "input_frame_ms") == 20U;
  } catch (...) {
    return false;
  }
}

}  // namespace

int main() {
  const std::string valid =
      R"({"version":1,"type":"hello.ack","message_id":"server-1","device_id":"00112233-4455-6677-8899-aabbccddeeff","session_id":1,"turn_id":0,"stream_id":0,"epoch":1,"payload":{"input_sample_rate_hz":16000,"output_sample_rate_hz":24000,"input_frame_ms":20}})";
  std::string invalid_utf8 = valid;
  invalid_utf8.insert(invalid_utf8.find("server-1"), "\xc3\x28", 2);
  std::string embedded_nul = valid;
  embedded_nul.insert(embedded_nul.find("server-1"), 1, '\0');

  const std::vector<std::pair<std::string, bool>> envelope_cases = {
      {valid, true}, {valid + " \n\t", true},
      {R"([])", false},
      {R"({"version":"1","type":"hello.ack","message_id":"m","device_id":"d","session_id":1,"turn_id":0,"stream_id":0,"epoch":1,"payload":{}})", false},
      {R"({"version":1,"version":1,"type":"hello.ack","message_id":"m","device_id":"d","session_id":1,"turn_id":0,"stream_id":0,"epoch":1,"payload":{}})", true},
      {R"({"version":1,"type":"hello.ack","message_id":"m","device_id":"d","session_id":1,"turn_id":0,"stream_id":0,"payload":{}})", false},
      {R"({"version":1,"type":"hello.ack","message_id":"m","device_id":"d","session_id":1,"turn_id":0,"stream_id":0,"epoch":1,"payload":{},"extra":0})", true},
      {R"({"version":1,"type":"hello.ack","message_id":"m","device_id":"d","session_id":-1,"turn_id":0,"stream_id":0,"epoch":1,"payload":{}})", false},
      {R"({"version":1,"type":"hello.ack","message_id":"m","device_id":"d","session_id":-0,"turn_id":0,"stream_id":0,"epoch":1,"payload":{}})", false},
      {R"({"version":1,"type":"hello.ack","message_id":"m","device_id":"d","session_id":1.5,"turn_id":0,"stream_id":0,"epoch":1,"payload":{}})", false},
      {R"({"version":1,"type":"hello.ack","message_id":"m","device_id":"d","session_id":1.0,"turn_id":0,"stream_id":0,"epoch":1,"payload":{}})", false},
      {R"({"version":1,"type":"hello.ack","message_id":"m","device_id":"d","session_id":1e0,"turn_id":0,"stream_id":0,"epoch":1,"payload":{}})", false},
      {R"({"version":1,"type":"hello.ack","message_id":"m","device_id":"d","session_id":4294967296,"turn_id":0,"stream_id":0,"epoch":1,"payload":{}})", false},
      {R"({"version":1,"type":"hello.ack","message_id":"m","device_id":"d","session_id":1,"turn_id":0,"stream_id":0,"epoch":1,"payload":[]})", false},
      {R"({"version":1,"type\u0000ignored":"hello.ack","message_id":"m","device_id":"d","session_id":1,"turn_id":0,"stream_id":0,"epoch":1,"payload":{}})", false},
      {R"({"version":1,"type":"hello.ack\u0000ignored","message_id":"m","device_id":"d","session_id":1,"turn_id":0,"stream_id":0,"epoch":1,"payload":{}})", false},
      {R"({"version":1,"type":"response.start","message_id":"m","device_id":"d","session_id":1,"turn_id":1,"stream_id":1,"epoch":1,"payload":{"response_id":"reply\u0000ignored"}})", false},
      {valid + "{}", false}, {invalid_utf8, false}, {embedded_nul, false},
  };

  int failures = 0;
  for (std::size_t index = 0; index < envelope_cases.size(); ++index) {
    if (AcceptEnvelope(envelope_cases[index].first) != envelope_cases[index].second) {
      std::cerr << "envelope case " << index << " failed\n";
      ++failures;
    }
  }
  const std::vector<std::string> invalid_payloads = {
      R"({"version":1,"type":"hello.ack","message_id":"m","device_id":"d","session_id":1,"turn_id":0,"stream_id":0,"epoch":1,"payload":{"input_sample_rate_hz":"16000","output_sample_rate_hz":24000,"input_frame_ms":20}})",
      R"({"version":1,"type":"hello.ack","message_id":"m","device_id":"d","session_id":1,"turn_id":0,"stream_id":0,"epoch":1,"payload":{"input_sample_rate_hz":16000,"output_sample_rate_hz":24000}})",
  };
  for (std::size_t index = 0; index < invalid_payloads.size(); ++index) {
    if (AcceptHelloAck(invalid_payloads[index])) {
      std::cerr << "payload case " << index << " failed\n";
      ++failures;
    }
  }
  const std::string compatible_payload =
      R"({"version":1,"type":"hello.ack","message_id":"m","device_id":"d","session_id":1,"turn_id":0,"stream_id":0,"epoch":1,"payload":{"input_sample_rate_hz":16000,"input_sample_rate_hz":16000,"output_sample_rate_hz":24000,"input_frame_ms":20,"future":true}})";
  if (!AcceptHelloAck(compatible_payload)) {
    std::cerr << "compatible duplicate/unknown payload fields were rejected\n";
    ++failures;
  }
  return failures == 0 ? 0 : 1;
}
