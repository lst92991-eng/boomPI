/** @file audio_format.h
 * @brief 定义采样率、通道数和业务帧长度，所有大小都从这组常量推导。
 * samples 指每通道的采样时刻数；交错数组元素数还需乘通道数。
 */
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

namespace audio
{
// 业务帧长20ms，用于语句计数和交付；设备采用48k格式，ALSA缓冲由声卡配置管理。
const unsigned kFrameMs = 20;
const unsigned kDeviceRateHz = 48000;
const unsigned kVoiceRateHz = 16000;
const std::size_t kCaptureChannels = 4;
const std::size_t kPlaybackChannels = 2;
const std::size_t kDeviceFrameSamples = kDeviceRateHz * kFrameMs / 1000;
const std::size_t kVoiceFrameSamples = kVoiceRateHz * kFrameMs / 1000;
typedef std::array<std::int16_t, kVoiceFrameSamples> VoiceFrame16k;
}  // namespace audio
