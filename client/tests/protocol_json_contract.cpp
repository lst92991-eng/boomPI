#include <cjson/cJSON.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "../src/network/voice_codec.h"

namespace {

void Check(bool condition, const std::string& message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

std::string Field(const cJSON* object, const char* key) {
  const auto* item = cJSON_GetObjectItemCaseSensitive(object, key);
  Check(cJSON_IsString(item) && item->valuestring, std::string("fixture string: ") + key);
  return item->valuestring;
}

std::uint32_t Number(const cJSON* object, const char* key) {
  const auto* item = cJSON_GetObjectItemCaseSensitive(object, key);
  Check(cJSON_IsNumber(item) && std::isfinite(item->valuedouble) && item->valuedouble >= 0 &&
            item->valuedouble <= UINT32_MAX &&
            std::floor(item->valuedouble) == item->valuedouble,
        std::string("fixture integer: ") + key);
  return static_cast<std::uint32_t>(item->valuedouble);
}

std::string HexBytes(const std::string& hex) {
  Check(hex.size() % 2 == 0, "fixture hex has odd length");
  std::string bytes;
  const std::string digits = "0123456789abcdef";
  for (std::size_t i = 0; i < hex.size(); i += 2) {
    const auto high = digits.find(hex[i]);
    const auto low = digits.find(hex[i + 1]);
    Check(high != std::string::npos && low != std::string::npos, "fixture has invalid hex");
    bytes.push_back(static_cast<char>((high << 4U) | low));
  }
  return bytes;
}

void SharedFixtures(const char* path) {
  using namespace boompi::network;
  std::ifstream input(path, std::ios::binary);
  Check(input.good(), std::string("cannot read shared fixture: ") + path);
  const std::string json{std::istreambuf_iterator<char>(input),
                         std::istreambuf_iterator<char>()};
  const char* end = nullptr;
  std::unique_ptr<cJSON, decltype(&cJSON_Delete)> root(
      cJSON_ParseWithLengthOpts(json.c_str(), json.size() + 1, &end, true), &cJSON_Delete);
  Check(root && end == json.c_str() + json.size() && Number(root.get(), "fixture_version") == 2,
        "invalid v2 fixture document");
  const auto* controls = cJSON_GetObjectItemCaseSensitive(root.get(), "control_frames");
  const auto* audio = cJSON_GetObjectItemCaseSensitive(root.get(), "audio_frames");
  Check(cJSON_IsArray(controls) && cJSON_IsArray(audio), "fixture frame arrays missing");
  unsigned control_count = 0, uplink_count = 0, downlink_count = 0;
  for (const auto* item = controls->child; item; item = item->next) {
    const auto* expected = cJSON_GetObjectItemCaseSensitive(item, "expected");
    const auto type = Field(expected, "type");
    // HELLO/STOP are uplink controls covered by the real TLS smoke test.
    if (type == "hello" || type == "stop") {
      continue;
    }
    const auto name = Field(item, "name");
    const auto event = detail::DecodeText(Field(item, "wire_text"));
    if (type == "ready") {
      Check(event.kind == LinkEventKind::Online && event.generation == 0,
            name + ": ready mismatch");
    } else {
      Check(event.generation == Number(expected, "generation"), name + ": generation mismatch");
      if (type == "text") {
        Check(event.kind == LinkEventKind::Text && event.text == Field(expected, "text"),
              name + ": text mismatch");
      } else if (type == "done") {
        Check(event.kind == LinkEventKind::Done, name + ": done mismatch");
      } else if (type == "error") {
        Check(event.kind == LinkEventKind::Error && event.code == Field(expected, "code"),
              name + ": error mismatch");
      } else {
        throw std::runtime_error(name + ": unsupported fixture control");
      }
    }
    ++control_count;
  }
  for (const auto* item = audio->child; item; item = item->next) {
    const auto name = Field(item, "name");
    const auto* header = cJSON_GetObjectItemCaseSensitive(item, "header");
    const auto flags = Number(header, "flags");
    const auto generation = Number(header, "generation");
    const auto sequence = Number(header, "sequence");
    const auto payload = HexBytes(Field(item, "payload_hex"));
    const auto wire = HexBytes(Field(item, "wire_hex"));
    Check(wire == HexBytes(Field(item, "header_hex")) + payload,
          name + ": inconsistent fixture bytes");
    const auto direction = Field(item, "direction");
    if (direction == "uplink") {
      Check(payload.size() == detail::kUplinkBytes, name + ": uplink size mismatch");
      std::array<std::int16_t, detail::kUplinkBytes / 2> pcm{};
      for (std::size_t i = 0; i < pcm.size(); ++i) {
        const auto value =
            static_cast<unsigned char>(payload[2 * i]) |
            (static_cast<unsigned>(static_cast<unsigned char>(payload[2 * i + 1])) << 8U);
        pcm[i] = static_cast<std::int16_t>(value < 32768 ? static_cast<int>(value)
                                                         : static_cast<int>(value) - 65536);
      }
      const auto encoded =
          detail::EncodeAudio(generation, sequence, pcm.data(), (flags & 1U) != 0,
                              (flags & 2U) != 0, (flags & 4U) != 0);
      Check(std::string(reinterpret_cast<const char*>(encoded.data()), encoded.size()) == wire,
            name + ": encoded bytes differ from shared golden wire");
      ++uplink_count;
    } else {
      Check(direction == "downlink", name + ": unknown direction");
      const auto event = detail::DecodeAudio(wire);
      Check(event.kind == LinkEventKind::Audio && event.generation == generation &&
                event.sequence == sequence && event.start == ((flags & 1U) != 0) &&
                event.end == ((flags & 2U) != 0) && event.audio_size == payload.size() &&
                std::string(reinterpret_cast<const char*>(event.audio.data()),
                            event.audio_size) == payload,
            name + ": decoded audio differs from shared golden fields");
      ++downlink_count;
    }
  }
  Check(control_count >= 4 && uplink_count >= 2 && downlink_count >= 3,
        "shared fixture coverage is incomplete");
  std::cout << "v2 shared fixture: " << control_count << " controls, " << uplink_count
            << " uplink, " << downlink_count << " downlink passed\n";
}

bool Accepted(const std::string& json) {
  try {
    (void)boompi::network::detail::DecodeText(json);
    return true;
  } catch (...) {
    return false;
  }
}

}  // namespace

int main(int argc, char** argv) {
  if (argc > 2) {
    std::cerr << "usage: protocol-json-test [shared-fixture.json]\n";
    return 1;
  }
  const std::string ready = R"({"type":"ready"})";
  const std::string text = R"({"type":"text","generation":1,"text":"你好"})";
  std::string invalid_utf8 = text;
  invalid_utf8.insert(invalid_utf8.find("你好"), "\xc3\x28", 2);
  std::string embedded_nul = text;
  embedded_nul.insert(embedded_nul.find("你好"), 1, '\0');
  const std::vector<std::pair<std::string, bool>> cases{
      {ready, true},
      {ready + " \n\t", true},
      {text, true},
      {R"({"type":"done","generation":4294967295})", true},
      {R"({"type":"error","generation":1,"code":"provider_timeout"})", true},
      {R"({"type":"text","generation":1,"text":"\\u0000"})", true},
      {R"({"type":"text","generation":1,"text":"\ud83d\ude42"})", true},
      {R"([])", false},
      {R"({"type":1})", false},
      {R"({"type":"ready","extra":0})", false},
      {R"({"type":"ready","type":"ready"})", false},
      {R"({"type":"ready","\u0074ype":"ready"})", false},
      {R"({"type\u0000hidden":"ready"})", false},
      {R"({"type":"ready\u0000hidden"})", false},
      {R"({"type":"done"})", false},
      {R"({"type":"done","generation":"1"})", false},
      {R"({"type":"done","generation":true})", false},
      {R"({"type":"done","generation":null})", false},
      {R"({"type":"done","generation":[]})", false},
      {R"({"type":"done","generation":{}})", false},
      {R"({"type":"done","generation":0})", false},
      {R"({"type":"done","generation":-0})", false},
      {R"({"type":"done","generation":-1})", false},
      {R"({"type":"done","generation":01})", false},
      {R"({"type":"done","generation":1.0})", false},
      {R"({"type":"done","generation":1e0})", false},
      {R"({"type":"done","generation":4294967296})", false},
      {R"({"type":"done","generation":1,"generation":1})", false},
      {R"({"type":"done","generation":1,"\u0067eneration":1})", false},
      {R"({"type":"text","generation":1,"text":false})", false},
      {R"({"type":"text","generation":1,"text":""})", false},
      {R"({"type":"text","generation":1,"text":"bad\qescape"})", false},
      {R"({"type":"text","generation":1,"text":"\u0000"})", false},
      {R"({"type":"text","generation":1,"text":"\ud800"})", false},
      {R"({"type":"text","generation":1,"text":"\udc00"})", false},
      {R"({"type":"text","generation":1,"text":"ok","other":{"x":1,"x":2}})", false},
      {R"({"type":"error","generation":1,"code":null})", false},
      {R"({"type":"error","generation":1,"code":"raw exception\n"})", false},
      {R"({"type":"hello"})", false},
      {R"({"version":1,"type":"hello.ack"})", false},
      {ready + "{}", false},
      {invalid_utf8, false},
      {embedded_nul, false},
      {R"({"type":"text","generation":1,"text":")" + std::string(4096, 'x') + "\"}", true},
      {R"({"type":"text","generation":1,"text":")" + std::string(4097, 'x') + "\"}", false},
      {ready + std::string(8192, ' '), false},
  };
  unsigned failures = 0;
  for (std::size_t i = 0; i < cases.size(); ++i) {
    if (Accepted(cases[i].first) != cases[i].second) {
      std::cerr << "v2 JSON case " << i << " failed\n";
      ++failures;
    }
  }
  if (argc == 2) {
    try {
      SharedFixtures(argv[1]);
    } catch (const std::exception& error) {
      std::cerr << error.what() << '\n';
      ++failures;
    }
  }
  return failures ? 1 : 0;
}
