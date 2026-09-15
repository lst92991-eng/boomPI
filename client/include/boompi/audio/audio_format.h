#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

namespace boompi::audio {
// 业务按20ms处理；这不是ALSA period。硬件16k双工未验，暂留48k设备格式。
inline constexpr unsigned kFrameMs = 20;
inline constexpr unsigned kDeviceRateHz = 48000;
inline constexpr unsigned kVoiceRateHz = 16000;
inline constexpr std::size_t kCaptureChannels = 4;
inline constexpr std::size_t kPlaybackChannels = 2;
inline constexpr std::size_t kDeviceFrameSamples = kDeviceRateHz * kFrameMs / 1000;
inline constexpr std::size_t kVoiceFrameSamples = kVoiceRateHz * kFrameMs / 1000;
using VoiceFrame16k = std::array<std::int16_t, kVoiceFrameSamples>;
}  // namespace boompi::audio
