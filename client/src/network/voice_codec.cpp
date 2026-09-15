#include "voice_codec.h"

#include <algorithm>
#include <charconv>
#include <cstring>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <websocketpp/utf8_validator.hpp>

namespace boompi::voice_net::detail {
namespace {
[[noreturn]] void Invalid() {
  throw std::runtime_error("invalid_protocol");
}
std::uint32_t Generation(std::string_view text) {
  if (text.empty() || text.front() == '0') {
    Invalid();
  }
  std::uint32_t value = 0;
  const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
  if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) {
    Invalid();
  }
  return value;
}
std::uint32_t ReadUint32(const std::uint8_t* bytes) {
  std::uint32_t value = 0;
  for (unsigned index = 0; index < 4; ++index) {
    value = (value << 8U) | bytes[index];
  }
  return value;
}

/// @brief 与 ReadUint32 配对写入头部；PCM 的小端编码在 EncodeAudio 中单独处理。
void WriteUint32(std::uint32_t value, std::uint8_t* bytes) {
  for (int index = 3; index >= 0; --index) {
    bytes[index] = static_cast<std::uint8_t>(value);
    value >>= 8U;
  }
}

}  // namespace

LinkEvent DecodeText(std::string text) {
  if (text.empty() || text.size() > 8192 || text.find('\0') != std::string::npos ||
      !websocketpp::utf8_validator::validate(text)) {
    Invalid();
  }
  LinkEvent event;
  if (text == "READY 4 16000") {
    event.kind = LinkEventKind::Online;
    return event;
  }
  const auto first = text.find(' ');
  if (first == std::string::npos) {
    Invalid();
  }
  const auto kind = text.substr(0, first);
  const auto second = text.find(' ', first + 1);
  const auto generation = std::string_view(text).substr(
      first + 1, second == std::string::npos ? second : second - first - 1);
  event.generation = Generation(generation);
  if (second != std::string::npos) {
    text.erase(0, second + 1);
    event.data = std::move(text);
  }
  if (kind == "DONE" && second == std::string::npos) {
    event.kind = LinkEventKind::Done;
  } else if (kind == "TEXT" && second != std::string::npos) {
    if (event.data.empty() || event.data.size() > 4096) {
      Invalid();
    }
    event.kind = LinkEventKind::Text;
  } else if (kind == "ERROR" && second != std::string::npos) {
    if (event.data.empty() || event.data.size() > 64) {
      Invalid();
    }
    for (unsigned char c : event.data) {
      if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) {
        Invalid();
      }
    }
    event.kind = LinkEventKind::Error;
  } else {
    Invalid();
  }
  return event;
}

LinkEvent DecodeAudio(std::string bytes) {
  if (bytes.size() < kHeaderBytes + 2 || bytes.size() > kFrameBytes || bytes.size() % 2 != 0) {
    Invalid();
  }
  const auto* header = reinterpret_cast<const std::uint8_t*>(bytes.data());
  if (std::memcmp(header, "BPV4", 4) != 0) {
    Invalid();
  }

  LinkEvent event;
  event.kind = LinkEventKind::Audio;
  event.generation = ReadUint32(header + 4);
  event.sequence = ReadUint32(header + 8);
  if (event.generation == 0 || event.sequence == UINT32_MAX) {
    Invalid();
  }
  bytes.erase(0, kHeaderBytes);
  event.data = std::move(bytes);
  return event;
}

std::array<std::uint8_t, kFrameBytes> EncodeAudio(std::uint32_t generation,
                                                  std::uint32_t sequence,
                                                  const audio::VoiceFrame16k& pcm) {
  std::array<std::uint8_t, kFrameBytes> bytes{};
  std::memcpy(bytes.data(), "BPV4", 4);
  WriteUint32(generation, bytes.data() + 4);
  WriteUint32(sequence, bytes.data() + 8);

  // 头部使用网络大端序，PCM 固定小端序；显式写字节，不依赖 CPU 端序或对齐。
  for (std::size_t index = 0; index < kPcmBytes / 2; ++index) {
    const std::uint16_t sample = static_cast<std::uint16_t>(pcm[index]);
    bytes[kHeaderBytes + 2 * index] = static_cast<std::uint8_t>(sample);
    bytes[kHeaderBytes + 2 * index + 1] = static_cast<std::uint8_t>(sample >> 8U);
  }
  return bytes;
}

}  // namespace boompi::voice_net::detail
