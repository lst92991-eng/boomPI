#pragma once

#include "boompi/network/voice_link.h"

#include <atomic>

namespace boompi::network::detail {

constexpr std::size_t kHeaderBytes = 16;
constexpr std::size_t kUplinkBytes = 640;
constexpr std::size_t kFrameBytes = kHeaderBytes + kUplinkBytes;
constexpr char kTeachingToken[] = "boompi-teaching-shared-token-v1-2026";

// 只完成单帧结构验证；跨帧generation与sequence由VoiceLink统一校验。
LinkEvent DecodeText(const std::string& json);
LinkEvent DecodeAudio(const std::string& bytes);
std::array<std::uint8_t, kFrameBytes> EncodeAudio(
    std::uint32_t generation, std::uint32_t sequence,
    const std::int16_t* pcm, bool start, bool end, bool supersede);
bool FindServer(const LinkConfig& configured, LinkConfig* found,
                const std::atomic<bool>* stop);

}  // namespace boompi::network::detail
