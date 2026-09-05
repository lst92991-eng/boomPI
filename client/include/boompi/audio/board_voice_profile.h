#pragma once

/**
 * @file board_voice_profile.h
 * @brief 当前 RV1106 教学板唯一的音频契约与维护者标定结果。
 *
 * 学生不通过环境变量修改这里的值。更换板卡、麦克风或模型时，维护者使用同一套
 * HIL 验收后整体更新 profile，避免把互相耦合的声学旋钮暴露到产品启动路径。
 */

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

  static constexpr std::size_t SamplesPerFrame(
      const std::uint32_t rate_hz) noexcept {
    return static_cast<std::size_t>(rate_hz) * frame_ms / 1000U;
  }

  static constexpr std::size_t FramesForMs(
      const std::uint32_t duration_ms) noexcept {
    return (duration_ms + frame_ms - 1U) / frame_ms;
  }
};

static_assert(VoiceFrameContract::capture_rate_hz *
                      VoiceFrameContract::frame_ms %
                  1000U ==
              0U,
              "capture frame must contain a whole number of samples");
static_assert(VoiceFrameContract::input_rate_hz * VoiceFrameContract::frame_ms %
                  1000U ==
              0U,
              "input frame must contain a whole number of samples");
static_assert(VoiceFrameContract::output_rate_hz *
                      VoiceFrameContract::frame_ms %
                  1000U ==
              0U,
              "output frame must contain a whole number of samples");

inline constexpr std::size_t kCaptureFrameSamples =
    VoiceFrameContract::SamplesPerFrame(VoiceFrameContract::capture_rate_hz);
inline constexpr std::size_t kVoiceFrameSamples =
    VoiceFrameContract::SamplesPerFrame(VoiceFrameContract::input_rate_hz);
inline constexpr std::size_t kTtsFrameSamples =
    VoiceFrameContract::SamplesPerFrame(VoiceFrameContract::output_rate_hz);

struct BoardVoiceProfile final {
  std::int8_t left_polarity;
  std::int8_t right_polarity;
  const char* wake_sensitivity;
  float vad_admission_dbfs;
  float barge_voice_dbfs;
  int aec_delay_samples;
};

#if defined(BOOMPI_ROCKCHIP_DELAY_SAMPLES)
inline constexpr int kBoardAecDelaySamples = BOOMPI_ROCKCHIP_DELAY_SAMPLES;
#else
inline constexpr int kBoardAecDelaySamples = 0;
#endif

inline constexpr BoardVoiceProfile kBoardVoiceProfile{
    1, 1, "0.7", -30.0F, -25.0F, kBoardAecDelaySamples};

static_assert(kBoardVoiceProfile.aec_delay_samples >= 0 &&
                  kBoardVoiceProfile.aec_delay_samples % 256 == 0,
              "Rockchip AEC delay must be a non-negative 256-sample multiple");

inline constexpr char kCapturePcm[] = "hw:0,0";
inline constexpr char kPlaybackPcm[] = "hw:0,0";
inline constexpr char kSnowboyResource[] =
    "/userdata/boompi/models/common.res";
inline constexpr char kSnowboyModel[] =
    "/userdata/boompi/models/snowboy.umdl";

}  // namespace boompi::audio
