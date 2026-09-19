/** @file voice_codec.cpp
 * @brief BPV4文本和PCM编码；格式错误返回false，轮次与连续性由网络接收入口校验。
 */
#include "voice_codec.h"

#include <algorithm>
#include <charconv>
#include <cstring>
#include <string_view>
#include <utility>
#include <websocketpp/utf8_validator.hpp>

namespace voice_codec
{
static bool Generation(std::string_view text, std::uint32_t &value)
{
    if (text.empty() || text.front() == '0')
    {
        return false;
    }
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
    {
        return false;
    }
    return true;
}
static std::uint32_t ReadUint32(const std::uint8_t *bytes)
{
    std::uint32_t value = 0;
    for (unsigned index = 0; index < 4; ++index)
    {
        value = (value << 8U) | bytes[index];
    }
    return value;
}

/// @brief 与 ReadUint32 配对写入头部；PCM 的小端编码在 EncodeAudio 中单独处理。
static void WriteUint32(std::uint32_t value, std::uint8_t *bytes)
{
    for (int index = 3; index >= 0; --index)
    {
        bytes[index] = static_cast<std::uint8_t>(value);
        value >>= 8U;
    }
}

bool DecodeText(std::string text, voice_net::LinkEvent &event)
{
    if (text.empty() || text.size() > 8192 || text.find('\0') != std::string::npos ||
        !websocketpp::utf8_validator::validate(text))
    {
        return false;
    }
    event = {};
    if (text == "READY 4 16000")
    {
        event.kind = voice_net::LinkEventKind::Online;
        return true;
    }
    const auto first = text.find(' ');
    if (first == std::string::npos)
    {
        return false;
    }
    const auto kind = text.substr(0, first);
    const auto second = text.find(' ', first + 1);
    const auto generation = std::string_view(text).substr(
        first + 1, second == std::string::npos ? second : second - first - 1);
    if (!Generation(generation, event.generation))
    {
        return false;
    }
    if (second != std::string::npos)
    {
        text.erase(0, second + 1);
        event.data = std::move(text);
    }
    if (kind == "DONE" && second == std::string::npos)
    {
        event.kind = voice_net::LinkEventKind::Done;
    }
    else if (kind == "TEXT" && second != std::string::npos)
    {
        if (event.data.empty() || event.data.size() > 4096)
        {
            return false;
        }
        event.kind = voice_net::LinkEventKind::Text;
    }
    else if (kind == "ERROR" && second != std::string::npos)
    {
        if (event.data.empty() || event.data.size() > 64)
        {
            return false;
        }
        for (unsigned char c : event.data)
        {
            if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'))
            {
                return false;
            }
        }
        event.kind = voice_net::LinkEventKind::Error;
    }
    else
    {
        return false;
    }
    return true;
}

bool DecodeAudio(std::string bytes, voice_net::LinkEvent &event)
{
    if (bytes.size() < kHeaderBytes + 2 || bytes.size() > kFrameBytes || bytes.size() % 2 != 0)
    {
        return false;
    }
    const auto *header = reinterpret_cast<const std::uint8_t *>(bytes.data());
    if (std::memcmp(header, "BPV4", 4) != 0)
    {
        return false;
    }

    event = {};
    event.kind = voice_net::LinkEventKind::Audio;
    event.generation = ReadUint32(header + 4);
    event.sequence = ReadUint32(header + 8);
    if (event.generation == 0 || event.sequence == UINT32_MAX)
    {
        return false;
    }
    bytes.erase(0, kHeaderBytes);
    event.data = std::move(bytes);
    return true;
}

std::array<std::uint8_t, kFrameBytes> EncodeAudio(std::uint32_t generation,
                                                  std::uint32_t sequence,
                                                  const audio::VoiceFrame16k &pcm)
{
    std::array<std::uint8_t, kFrameBytes> bytes{};
    std::memcpy(bytes.data(), "BPV4", 4);
    WriteUint32(generation, bytes.data() + 4);
    WriteUint32(sequence, bytes.data() + 8);

    // 头部使用网络大端序，PCM 固定小端序；显式写字节，不依赖 CPU 端序或对齐。
    for (std::size_t index = 0; index < kPcmBytes / 2; ++index)
    {
        const std::uint16_t sample = static_cast<std::uint16_t>(pcm[index]);
        bytes[kHeaderBytes + 2 * index] = static_cast<std::uint8_t>(sample);
        bytes[kHeaderBytes + 2 * index + 1] = static_cast<std::uint8_t>(sample >> 8U);
    }
    return bytes;
}

}  // namespace voice_codec
