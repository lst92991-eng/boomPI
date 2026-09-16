/** @file audio_convert.cpp
 * @brief 当前硬件格式与算法格式之间的适配；两个方向各自保留滤波历史。
 *
 * 采集：校正双麦极性 → 双麦/refL共同48k→16k。
 * 播放：16k单声道 → 48k交错双声道；结束时单独取出滤波尾音。
 * 两个 SwrContext 不共享采样。16k 硬件全双工尚待验证，因此保留当前转换。
 */
#include "audio_convert.h"

#include "board_voice_profile.h"

extern "C" {
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
}

#include <algorithm>

namespace boompi::audio_convert {
namespace {
// capture和playback各自只由对应线程使用，启动/退出时分别开关，不共享滤波历史。
SwrContext* capture_swr{nullptr};
SwrContext* playback_swr{nullptr};
std::size_t playback_pending_frames{0U};

/** @brief 只分配 S16 交错转换器；矩阵设置及初始化由对应方向的 open/reset 完成。 */
SwrContext* create_resampler(int in_rate, int in_channels, int out_rate,
                             int out_channels) noexcept {
  return swr_alloc_set_opts(
      nullptr, av_get_default_channel_layout(out_channels), AV_SAMPLE_FMT_S16, out_rate,
      av_get_default_channel_layout(in_channels), AV_SAMPLE_FMT_S16, in_rate, 0, nullptr);
}
}  // namespace

bool open_capture() noexcept {
  capture_swr = create_resampler(audio::kDeviceRateHz, 4, audio::kVoiceRateHz, 3);
  // 每行对应一个输出通道；最后一列全零，refR不进入算法。
  // clang-format off
  const double channels[] = {
      audio::board::kLeftMicPolarity, 0, 0, 0,   // mic0
      0, audio::board::kRightMicPolarity, 0, 0,  // mic1
      0, 0, 1, 0                               // refL
  };
  // clang-format on
  if (!capture_swr || swr_set_matrix(capture_swr, channels, 4) < 0 || !reset_capture()) {
    close_capture();
    return false;
  }
  return true;
}

bool open_playback() noexcept {
  playback_swr = create_resampler(audio::kVoiceRateHz, 1, audio::kDeviceRateHz, 2);
  // 明确L=mono、R=mono，避免默认声道矩阵衰减；矩阵在初始化前设置。
  const double stereo_matrix[] = {1.0, 1.0};
  if (!playback_swr || swr_set_matrix(playback_swr, stereo_matrix, 1) < 0 ||
      swr_init(playback_swr) < 0) {
    close_playback();
    return false;
  }
  return true;
}

bool reset_capture() noexcept {
  swr_close(capture_swr);
  if (swr_init(capture_swr) < 0) {
    return false;
  }
  // 送入一块静音建立滤波历史，后续每次960点输入才能稳定交付320点。
  const audio::RawCaptureFrame silence{};
  const std::uint8_t* in[] = {reinterpret_cast<const std::uint8_t*>(silence.data())};
  audio::CaptureChannels discarded;
  std::uint8_t* out[] = {reinterpret_cast<std::uint8_t*>(discarded.data())};
  return swr_convert(capture_swr, out, audio::kVoiceFrameSamples, in,
                     audio::kDeviceFrameSamples) >= 0;
}

bool reset_playback() noexcept {
  swr_close(playback_swr);
  playback_pending_frames = 0U;
  return swr_init(playback_swr) >= 0;
}

bool capture(const audio::RawCaptureFrame& raw, audio::CaptureChannels& output) noexcept {
  // 直接消费原始交错PCM，库内一次完成选通道、极性与共同重采样。
  const std::uint8_t* in[] = {reinterpret_cast<const std::uint8_t*>(raw.data())};
  std::uint8_t* out[] = {reinterpret_cast<std::uint8_t*>(output.data())};
  const int converted =
      swr_convert(capture_swr, out, audio::kVoiceFrameSamples, in, audio::kDeviceFrameSamples);
  return converted == static_cast<int>(audio::kVoiceFrameSamples);
}

bool playback(const std::int16_t* pcm, std::size_t samples,
              audio::StereoPlaybackFrame& output) noexcept {
  output.frames = 0U;
  // FFmpeg的null flush可能吞掉不足滤波半窗的极短输入。
  // EOS送入只读静音来推进滤波，但输出上限仅为尚欠的有效采样时刻，不播放补齐静音。
  static constexpr std::array<std::int16_t, audio::kVoiceFrameSamples> silence{};
  constexpr std::size_t ratio = audio::kDeviceRateHz / audio::kVoiceRateHz;
  // 当前 48k/16k 比例为整数 3；有效输出时刻只按真实输入计数，补齐静音不延长回答。
  playback_pending_frames += samples * ratio;
  if (playback_pending_frames > 2 * audio::kPlaybackFrameCapacity) {
    return false;
  }
  if (playback_pending_frames == 0U) {
    return true;
  }
  const bool ending = pcm == nullptr;
  if (ending) {
    pcm = silence.data();
    samples = silence.size();
  }
  const std::uint8_t* in[] = {reinterpret_cast<const std::uint8_t*>(pcm)};
  std::uint8_t* out[] = {reinterpret_cast<std::uint8_t*>(output.pcm.data())};
  const int converted = swr_convert(
      playback_swr, out,
      static_cast<int>(std::min(playback_pending_frames, audio::kPlaybackFrameCapacity)), in,
      static_cast<int>(samples));
  if (converted < 0 || (ending && converted == 0)) {
    return false;
  }
  output.frames = static_cast<std::size_t>(converted);
  playback_pending_frames -= output.frames;
  return true;
}

void close_capture() noexcept {
  swr_free(&capture_swr);
}

void close_playback() noexcept {
  swr_free(&playback_swr);
  playback_pending_frames = 0U;
}

}  // namespace boompi::audio_convert
