#pragma once
#include <array>

#include "boompi/network/voice_net.h"

namespace voice_codec
{

const std::size_t kHeaderBytes = 12;
const std::size_t kPcmBytes = 640;
const std::size_t kFrameBytes = kHeaderBytes + kPcmBytes;
voice_net::LinkEvent DecodeText(std::string text);
voice_net::LinkEvent DecodeAudio(std::string bytes);
std::array<std::uint8_t, kFrameBytes> EncodeAudio(std::uint32_t generation,
                                                  std::uint32_t sequence,
                                                  const audio::VoiceFrame16k &pcm);

}  // namespace voice_codec
