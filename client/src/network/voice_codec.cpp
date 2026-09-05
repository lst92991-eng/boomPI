#include "voice_codec.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string_view>

#include <cjson/cJSON.h>
#include <websocketpp/utf8_validator.hpp>

namespace boompi::network::detail {
namespace {

[[noreturn]] void Invalid() { throw std::runtime_error("invalid_protocol"); }

void ExactFields(const cJSON* root,
                 std::initializer_list<std::string_view> names) {
  if (!cJSON_IsObject(root)) Invalid();
  std::size_t count = 0;
  for (const cJSON* item = root->child; item; item = item->next) {
    if (!item->string || std::find(names.begin(), names.end(), item->string) == names.end()) Invalid();
    for (const cJSON* prior = root->child; prior != item; prior = prior->next)
      if (std::strcmp(prior->string, item->string) == 0) Invalid();
    ++count;
  }
  if (count != names.size()) Invalid();
}

std::string String(const cJSON* root, const char* key, std::size_t maximum) {
  const cJSON* item = cJSON_GetObjectItemCaseSensitive(root, key);
  if (!cJSON_IsString(item) || !item->valuestring) Invalid();
  std::string value(item->valuestring);
  if (value.empty() || value.size() > maximum ||
      !websocketpp::utf8_validator::validate(value)) Invalid();
  return value;
}

// cJSON会将\u0000截短并把指数数字转为double，因此先约束原始词法。
// 无符号字段只接受十进制整数，字符内容必须使用合法JSON转义。
void ValidateSyntax(const std::string& json) {
  bool quoted = false;
  for (std::size_t index = 0; index < json.size(); ++index) {
    const unsigned char c = static_cast<unsigned char>(json[index]);
    if (quoted) {
      if (c < 0x20U) Invalid();
      if (c == '"') quoted = false;
      if (c != '\\') continue;
      if (++index == json.size()) Invalid();
      const char escaped = json[index];
      if (escaped == 'u') {
        if (index + 4 >= json.size()) Invalid();
        if (json.compare(index + 1, 4, "0000") == 0) Invalid();
        index += 4;
      } else if (std::string_view("\"\\/bfnrt").find(escaped) == std::string_view::npos) Invalid();
    } else if (c == '"') {
      quoted = true;
    } else if (c >= '0' && c <= '9') {
      const auto first = index;
      while (index + 1 < json.size() && json[index + 1] >= '0' && json[index + 1] <= '9') ++index;
      if (json[first] == '0' && index != first) Invalid();
      if (index + 1 < json.size() && std::string_view(".eE").find(json[index + 1]) != std::string_view::npos) Invalid();
    } else if (c == '-' || c == '+' || c == 0) {
      Invalid();
    }
  }
}

std::uint32_t Get32(const std::uint8_t* p) {
  std::uint32_t value = 0;
  for (unsigned i = 0; i < 4; ++i) value = (value << 8U) | p[i];
  return value;
}
void Put32(std::uint32_t value, std::uint8_t* p) {
  for (int i = 3; i >= 0; --i) { p[i] = static_cast<std::uint8_t>(value); value >>= 8U; }
}

}  // namespace

LinkEvent DecodeText(const std::string& json) {
  if (json.empty() || json.size() > 8192 || !websocketpp::utf8_validator::validate(json)) Invalid();
  ValidateSyntax(json);
  const char* end = nullptr;
  std::unique_ptr<cJSON, decltype(&cJSON_Delete)> root(
      cJSON_ParseWithLengthOpts(json.c_str(), json.size() + 1, &end, true), &cJSON_Delete);
  if (!root || end != json.c_str() + json.size()) Invalid();
  const auto type = String(root.get(), "type", 16);
  LinkEvent event;
  if (type == "ready") {
    ExactFields(root.get(), {"type"});
    event.kind = LinkEventKind::Online;
    return event;
  }
  if (type == "text") {
    ExactFields(root.get(), {"type", "generation", "text"});
    event.kind = LinkEventKind::Text;
    event.text = String(root.get(), "text", 4096);
  } else if (type == "done") {
    ExactFields(root.get(), {"type", "generation"});
    event.kind = LinkEventKind::Done;
  } else if (type == "error") {
    ExactFields(root.get(), {"type", "generation", "code"});
    event.kind = LinkEventKind::Error;
    event.code = String(root.get(), "code", 64);
    for (unsigned char c : event.code)
      if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) Invalid();
  } else Invalid();
  const auto* generation = cJSON_GetObjectItemCaseSensitive(root.get(), "generation");
  if (!cJSON_IsNumber(generation) || !std::isfinite(generation->valuedouble) ||
      generation->valuedouble < 1 || generation->valuedouble > UINT32_MAX ||
      std::trunc(generation->valuedouble) != generation->valuedouble) Invalid();
  event.generation = static_cast<std::uint32_t>(generation->valuedouble);
  return event;
}

LinkEvent DecodeAudio(const std::string& bytes) {
  if (bytes.size() < kHeaderBytes + 2 || bytes.size() > kHeaderBytes + 960 || bytes.size() % 2) Invalid();
  const auto* p = reinterpret_cast<const std::uint8_t*>(bytes.data());
  if (std::memcmp(p, "BPV2", 4) != 0 || p[4] || (p[5] & ~3U) || p[6] || p[7]) Invalid();
  LinkEvent event;
  event.kind = LinkEventKind::Audio;
  event.generation = Get32(p + 8);
  event.sequence = Get32(p + 12);
  event.start = (p[5] & 1U) != 0;
  event.end = (p[5] & 2U) != 0;
  event.audio_size = bytes.size() - kHeaderBytes;
  if (!event.generation || event.sequence == UINT32_MAX ||
      (event.start != (event.sequence == 0)) || (!event.end && event.audio_size != 960)) Invalid();
  std::copy_n(p + kHeaderBytes, event.audio_size, event.audio.begin());
  return event;
}

std::array<std::uint8_t, kFrameBytes> EncodeAudio(
    std::uint32_t generation, std::uint32_t sequence, const std::int16_t* pcm,
    bool start, bool end, bool supersede) {
  std::array<std::uint8_t, kFrameBytes> bytes{};
  std::memcpy(bytes.data(), "BPV2", 4);
  bytes[5] = static_cast<std::uint8_t>((start ? 1U : 0U) | (end ? 2U : 0U) | (supersede ? 4U : 0U));
  Put32(generation, bytes.data() + 8);
  Put32(sequence, bytes.data() + 12);
  for (std::size_t i = 0; i < kUplinkBytes / 2; ++i) {
    const auto sample = static_cast<std::uint16_t>(pcm[i]);
    bytes[kHeaderBytes + 2 * i] = static_cast<std::uint8_t>(sample);
    bytes[kHeaderBytes + 2 * i + 1] = static_cast<std::uint8_t>(sample >> 8U);
  }
  return bytes;
}

}  // namespace boompi::network::detail
