/** @file audio_format.h
 * @brief 定义采样率、通道数和业务帧长度，所有大小都从这组常量推导。
 * samples 指每通道的采样时刻数；交错数组元素数还需乘通道数。
 */
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

namespace boompi::audio {
// 业务按20ms处理；这不是ALSA period。硬件16k双工未验，暂留48k设备格式。
constexpr unsigned kFrameMs = 20;
constexpr unsigned kDeviceRateHz = 48000;
constexpr unsigned kVoiceRateHz = 16000;
constexpr std::size_t kCaptureChannels = 4;
constexpr std::size_t kPlaybackChannels = 2;
constexpr std::size_t kDeviceFrameSamples = kDeviceRateHz * kFrameMs / 1000;
constexpr std::size_t kVoiceFrameSamples = kVoiceRateHz * kFrameMs / 1000;
using VoiceFrame16k = std::array<std::int16_t, kVoiceFrameSamples>;
}  // namespace boompi::audio
