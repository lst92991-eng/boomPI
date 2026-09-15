/** @file audio_format.h
 * @brief 客户端音频使用的固定采样率与 20 ms 帧长。
 *
 * ALSA 每通道 960 样本 → 上行 320 样本；下行 TTS 每帧 320 样本 → 播放每通道
 * 960 样本。这里的样本数均按单声道计算，交错缓冲容量还要乘以通道数。
 */
#pragma once

#include <cstddef>
#include <cstdint>

namespace boompi::audio {

/// @brief 本地采集、线上 PCM 与播放共用的时间单位；修改后须同步验证协议与板端库。
struct VoiceFrameContract final {
  static constexpr std::uint32_t frame_ms = 20U;
  static constexpr std::uint32_t capture_rate_hz = 48000U;
  static constexpr std::uint32_t input_rate_hz = 16000U;
  static constexpr std::uint32_t output_rate_hz = 16000U;
  static constexpr std::size_t capture_channels = 4U;
  static constexpr std::size_t playback_channels = 2U;

  /// 将采样率换算为每通道 20 ms 的样本数；当前采样率均可整除。
  static constexpr std::size_t SamplesPerFrame(std::uint32_t rate_hz) noexcept {
    return static_cast<std::size_t>(rate_hz) * frame_ms / 1000U;
  }

  /// 向上取整，持续时间不足一帧时仍保留一个完整采集周期。
  static constexpr std::size_t FramesForMs(std::uint32_t duration_ms) noexcept {
    return (duration_ms + frame_ms - 1U) / frame_ms;
  }
};

static_assert(VoiceFrameContract::capture_rate_hz * VoiceFrameContract::frame_ms % 1000U == 0U,
              "capture frame must contain a whole number of samples");
static_assert(VoiceFrameContract::input_rate_hz * VoiceFrameContract::frame_ms % 1000U == 0U,
              "input frame must contain a whole number of samples");
static_assert(VoiceFrameContract::output_rate_hz * VoiceFrameContract::frame_ms % 1000U == 0U,
              "output frame must contain a whole number of samples");

/// 原始采集和最终播放的每通道 period 长度：48 kHz×20 ms = 960。
inline constexpr std::size_t kCaptureFrameSamples =
    VoiceFrameContract::SamplesPerFrame(VoiceFrameContract::capture_rate_hz);
/// 3A、Snowboy、VAD 和上行协议共同使用的单声道帧长：16 kHz×20 ms = 320。
inline constexpr std::size_t kVoiceFrameSamples =
    VoiceFrameContract::SamplesPerFrame(VoiceFrameContract::input_rate_hz);
/// 下行 TTS 的完整单声道帧长：16 kHz×20 ms = 320；仅最后一帧允许更短。
inline constexpr std::size_t kTtsFrameSamples =
    VoiceFrameContract::SamplesPerFrame(VoiceFrameContract::output_rate_hz);

}  // namespace boompi::audio
