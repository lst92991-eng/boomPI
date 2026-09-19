#pragma once
#include <array>

#include "boompi/network/voice_net.h"

namespace voice_codec
{

const std::size_t kHeaderBytes = 12;
const std::size_t kPcmBytes = 640;
const std::size_t kFrameBytes = kHeaderBytes + kPcmBytes;
// 成功时event拥有消息负载；false表示格式错误，调用方丢弃输出并断开连接。
bool DecodeText(std::string text, voice_net::LinkEvent &event);
bool DecodeAudio(std::string bytes, voice_net::LinkEvent &event);
std::array<std::uint8_t, kFrameBytes> EncodeAudio(std::uint32_t generation,
                                                  std::uint32_t sequence,
                                                  const audio::VoiceFrame16k &pcm);

}  // namespace voice_codec
