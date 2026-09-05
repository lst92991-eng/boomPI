/**
 * @file voice_codec.cpp
 * @brief v2 单帧编解码：检查 JSON 形状、PCM 格式和字节序。
 *
 * 这里只判断一帧是否合法；是否属于当前 generation、序号是否连续由 VoiceLink
 * 判断。解析失败统一抛出阶段错误，原始消息和凭据不会进入错误文本。
 */
#include "voice_codec.h"

#include <cjson/cJSON.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <initializer_list>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <websocketpp/utf8_validator.hpp>

namespace boompi::network::detail {
namespace {

[[noreturn]] void Invalid() {
  throw std::runtime_error("invalid_protocol");
}

/// @brief 每种消息只接受约定字段；重复键不能由 JSON 库自行挑选一个值。
void ExactFields(const cJSON* root, std::initializer_list<std::string_view> names) {
  if (!cJSON_IsObject(root)) {
    Invalid();
  }
  std::size_t count = 0;
  for (const cJSON* item = root->child; item != nullptr; item = item->next) {
    if (item->string == nullptr ||
        std::find(names.begin(), names.end(), item->string) == names.end()) {
      Invalid();
    }
    for (const cJSON* prior = root->child; prior != item; prior = prior->next) {
      if (std::strcmp(prior->string, item->string) == 0) {
        Invalid();
      }
    }
    ++count;
  }
  if (count != names.size()) {
    Invalid();
  }
}

std::string ReadString(const cJSON* root, const char* key, std::size_t maximum) {
  const cJSON* item = cJSON_GetObjectItemCaseSensitive(root, key);
  if (!cJSON_IsString(item) || item->valuestring == nullptr) {
    Invalid();
  }
  std::string value(item->valuestring);
  if (value.empty() || value.size() > maximum ||
      !websocketpp::utf8_validator::validate(value)) {
    Invalid();
  }
  return value;
}

/**
 * @brief 在 cJSON 转换数值和字符串前，排除会产生歧义的原始写法。
 *
 * cJSON 会把转义 NUL 截短、把指数数字转为 double。v2 的数值只接受无符号
 * 十进制整数；字符串转义的其余合法性继续交给 cJSON 检查。
 */
void ValidateSyntax(const std::string& json) {
  bool quoted = false;
  for (std::size_t index = 0; index < json.size(); ++index) {
    const unsigned char byte = static_cast<unsigned char>(json[index]);
    if (quoted) {
      if (byte < 0x20U) {
        Invalid();
      }
      if (byte == '"') {
        quoted = false;
      }
      if (byte != '\\') {
        continue;
      }
      ++index;
      if (index == json.size()) {
        Invalid();
      }
      const char escaped = json[index];
      if (escaped == 'u') {
        if (index + 4 >= json.size() || json.compare(index + 1, 4, "0000") == 0) {
          Invalid();
        }
        index += 4;
      } else if (std::string_view("\"\\/bfnrt").find(escaped) == std::string_view::npos) {
        Invalid();
      }
      continue;
    }

    if (byte == '"') {
      quoted = true;
    } else if (byte >= '0' && byte <= '9') {
      const std::size_t first = index;
      while (index + 1 < json.size() && json[index + 1] >= '0' && json[index + 1] <= '9') {
        ++index;
      }
      if (json[first] == '0' && index != first) {
        Invalid();
      }
      if (index + 1 < json.size() &&
          std::string_view(".eE").find(json[index + 1]) != std::string_view::npos) {
        Invalid();
      }
    } else if (byte == '-' || byte == '+' || byte == 0) {
      Invalid();
    }
  }
}

std::uint32_t ReadUint32(const std::uint8_t* bytes) {
  std::uint32_t value = 0;
  for (unsigned index = 0; index < 4; ++index) {
    value = (value << 8U) | bytes[index];
  }
  return value;
}

void WriteUint32(std::uint32_t value, std::uint8_t* bytes) {
  for (int index = 3; index >= 0; --index) {
    bytes[index] = static_cast<std::uint8_t>(value);
    value >>= 8U;
  }
}

}  // namespace

LinkEvent DecodeText(const std::string& json) {
  if (json.empty() || json.size() > 8192 || !websocketpp::utf8_validator::validate(json)) {
    Invalid();
  }
  ValidateSyntax(json);
  const char* end = nullptr;
  // 校验途中可能抛出异常，智能指针仍会调用 cJSON_Delete 释放解析树。
  std::unique_ptr<cJSON, decltype(&cJSON_Delete)> root(
      cJSON_ParseWithLengthOpts(json.c_str(), json.size() + 1, &end, true), &cJSON_Delete);
  if (!root || end != json.c_str() + json.size()) {
    Invalid();
  }

  const std::string type = ReadString(root.get(), "type", 16);
  LinkEvent event;
  if (type == "ready") {
    ExactFields(root.get(), {"type"});
    event.kind = LinkEventKind::Online;
    return event;
  }
  if (type == "text") {
    ExactFields(root.get(), {"type", "generation", "text"});
    event.kind = LinkEventKind::Text;
    event.text = ReadString(root.get(), "text", 4096);
  } else if (type == "done") {
    ExactFields(root.get(), {"type", "generation"});
    event.kind = LinkEventKind::Done;
  } else if (type == "error") {
    ExactFields(root.get(), {"type", "generation", "code"});
    event.kind = LinkEventKind::Error;
    event.code = ReadString(root.get(), "code", 64);
    for (const unsigned char byte : event.code) {
      const bool valid =
          (byte >= 'a' && byte <= 'z') || (byte >= '0' && byte <= '9') || byte == '_';
      if (!valid) {
        Invalid();
      }
    }
  } else {
    Invalid();
  }

  const cJSON* generation = cJSON_GetObjectItemCaseSensitive(root.get(), "generation");
  if (!cJSON_IsNumber(generation) || !std::isfinite(generation->valuedouble) ||
      generation->valuedouble < 1 || generation->valuedouble > UINT32_MAX ||
      std::trunc(generation->valuedouble) != generation->valuedouble) {
    Invalid();
  }
  event.generation = static_cast<std::uint32_t>(generation->valuedouble);
  return event;
}

LinkEvent DecodeAudio(const std::string& bytes) {
  if (bytes.size() < kHeaderBytes + 2 || bytes.size() > kHeaderBytes + 960 ||
      bytes.size() % 2 != 0) {
    Invalid();
  }
  const auto* header = reinterpret_cast<const std::uint8_t*>(bytes.data());
  if (std::memcmp(header, "BPV2", 4) != 0 || header[4] != 0 || (header[5] & ~3U) != 0 ||
      header[6] != 0 || header[7] != 0) {
    Invalid();
  }

  LinkEvent event;
  event.kind = LinkEventKind::Audio;
  event.generation = ReadUint32(header + 8);
  event.sequence = ReadUint32(header + 12);
  event.start = (header[5] & 1U) != 0;
  event.end = (header[5] & 2U) != 0;
  event.audio_size = bytes.size() - kHeaderBytes;
  if (event.generation == 0 || event.sequence == UINT32_MAX ||
      event.start != (event.sequence == 0) || (!event.end && event.audio_size != 960)) {
    Invalid();
  }
  std::copy_n(header + kHeaderBytes, event.audio_size, event.audio.begin());
  return event;
}

std::array<std::uint8_t, kFrameBytes> EncodeAudio(std::uint32_t generation,
                                                  std::uint32_t sequence,
                                                  const std::int16_t* pcm, bool start, bool end,
                                                  bool supersede) {
  std::array<std::uint8_t, kFrameBytes> bytes{};
  std::memcpy(bytes.data(), "BPV2", 4);
  if (start) {
    bytes[5] |= 1U;
  }
  if (end) {
    bytes[5] |= 2U;
  }
  if (supersede) {
    bytes[5] |= 4U;
  }
  WriteUint32(generation, bytes.data() + 8);
  WriteUint32(sequence, bytes.data() + 12);

  // 头部使用网络大端序，PCM 固定小端序；显式写字节，不依赖 CPU 端序或对齐。
  for (std::size_t index = 0; index < kUplinkBytes / 2; ++index) {
    const std::uint16_t sample = static_cast<std::uint16_t>(pcm[index]);
    bytes[kHeaderBytes + 2 * index] = static_cast<std::uint8_t>(sample);
    bytes[kHeaderBytes + 2 * index + 1] = static_cast<std::uint8_t>(sample >> 8U);
  }
  return bytes;
}

}  // namespace boompi::network::detail
