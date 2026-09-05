/** @file audio_format.h
 * @brief 客户端音频使用的固定采样率与 20 ms 帧长。
 */
#pragma once

#include <cstddef>
#include <cstdint>

namespace boompi::audio {

struct VoiceFrameContract final {
  static constexpr std::uint32_t frame_ms = 20U;
  static constexpr std::uint32_t capture_rate_hz = 48000U;
  static constexpr std::uint32_t input_rate_hz = 16000U;
  static constexpr std::uint32_t output_rate_hz = 24000U;
  static constexpr std::size_t capture_channels = 4U;
  static constexpr std::size_t playback_channels = 2U;

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

inline constexpr std::size_t kCaptureFrameSamples =
    VoiceFrameContract::SamplesPerFrame(VoiceFrameContract::capture_rate_hz);
inline constexpr std::size_t kVoiceFrameSamples =
    VoiceFrameContract::SamplesPerFrame(VoiceFrameContract::input_rate_hz);
inline constexpr std::size_t kTtsFrameSamples =
    VoiceFrameContract::SamplesPerFrame(VoiceFrameContract::output_rate_hz);

}  // namespace boompi::audio
