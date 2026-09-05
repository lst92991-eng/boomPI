#pragma once

/**
 * @file voice_codec.h
 * @brief 网络模块私有的 v2 单帧编解码声明。
 */

#include "boompi/network/voice_link.h"

namespace boompi::network::detail {

constexpr std::size_t kHeaderBytes = 16;
constexpr std::size_t kUplinkBytes = 640;
constexpr std::size_t kFrameBytes = kHeaderBytes + kUplinkBytes;
// 单帧结构验证失败时抛出协议错误；跨帧 generation 与 sequence 由 VoiceLink 校验。
LinkEvent DecodeText(const std::string& json);
LinkEvent DecodeAudio(const std::string& bytes);
std::array<std::uint8_t, kFrameBytes> EncodeAudio(std::uint32_t generation,
                                                  std::uint32_t sequence,
                                                  const std::int16_t* pcm, bool start, bool end,
                                                  bool supersede);

}  // namespace boompi::network::detail
