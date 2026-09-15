#pragma once
#include <array>

#include "boompi/network/voice_net.h"

namespace boompi::voice_net::detail {

constexpr std::size_t kHeaderBytes = 12;
constexpr std::size_t kPcmBytes = 640;
constexpr std::size_t kFrameBytes = kHeaderBytes + kPcmBytes;
LinkEvent DecodeText(std::string text);
LinkEvent DecodeAudio(std::string bytes);
std::array<std::uint8_t, kFrameBytes> EncodeAudio(std::uint32_t generation,
                                                  std::uint32_t sequence,
                                                  const audio::VoiceFrame16k& pcm);

}  // namespace boompi::voice_net::detail
