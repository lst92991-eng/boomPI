/** @file audio_converter.cpp
 * @brief 采集端保持麦克风与参考同相位，播放端保持 20 ms 的重采样节拍。
 *
 * 采集：校正双麦极性 → 四通道共同 48k→16k → 拆出双麦/refL → 计算准入元数据。
 * 播放：16k单声道 → 48k交错双声道 → 峰值限制/音量；EOS单独取出滤波尾音。
 * 两条方向各持有独立 SwrContext；Open/Close 由启动/退出路径串行管理。
 */
#include "audio_converter.h"

extern "C" {
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
}

#include <algorithm>
#include <cmath>
#include <limits>

namespace boompi::platform::rv1106 {
namespace {
using audio::VoiceFrameContract;
constexpr std::size_t kCaptureChannels = VoiceFrameContract::capture_channels;
constexpr int kReferencePeakThreshold = 64;
constexpr float kPlaybackPeakLimit = 31128.0F;  // 95% 满幅，为功放保留余量。

/** @brief 创建 S16 交错格式转换器；失败返回空指针，成功句柄由 AudioConverter 回收。 */
SwrContext* NewResampler(int in_rate, int in_channels, int out_rate,
                         int out_channels) noexcept {
  SwrContext* swr = swr_alloc_set_opts(
      nullptr, av_get_default_channel_layout(out_channels), AV_SAMPLE_FMT_S16, out_rate,
      av_get_default_channel_layout(in_channels), AV_SAMPLE_FMT_S16, in_rate, 0, nullptr);
  if (swr == nullptr || swr_init(swr) < 0) {
    swr_free(&swr);
  }
  return swr;
}

/** @brief 清掉旧滤波历史并送入一帧静音；只在所属线程的控制边界调用。 */
bool PrimeResampler(SwrContext* swr, const std::int16_t* input, int input_frames,
                    int output_frames, std::int16_t* output) noexcept {
  if (swr == nullptr) {
    return false;
  }
  swr_close(swr);
  if (swr_init(swr) < 0) {
    return false;
  }
  const std::uint8_t* in[] = {reinterpret_cast<const std::uint8_t*>(input)};
  std::uint8_t* out[] = {reinterpret_cast<std::uint8_t*>(output)};
  return swr_convert(swr, out, output_frames, in, input_frames) >= 0;
}
}  // namespace

/** @brief 去掉直流偏置再计算相对满幅电平，避免偏置抬高 VAD 准入值。 */
float AcRmsDbfs(const audio::VoiceFrame16k& samples) noexcept {
  double sum = 0.0;
  for (const std::int16_t sample : samples) {
    sum += sample;
  }
  const double mean = sum / static_cast<double>(samples.size());
  double energy = 0.0;
  for (const std::int16_t sample : samples) {
    const double centered = static_cast<double>(sample) - mean;
    energy += centered * centered;
  }
  if (energy <= 0.0) {
    return -120.0F;
  }
  const double rms = std::sqrt(energy / static_cast<double>(samples.size()));
  return static_cast<float>(20.0 * std::log10(rms / 32768.0));
}

AudioConverter::~AudioConverter() noexcept {
  Close();
}

bool AudioConverter::Open(std::int8_t left_polarity, std::int8_t right_polarity) noexcept {
  if (capture_swr_ != nullptr || playback_swr_ != nullptr ||
      (left_polarity != 1 && left_polarity != -1) ||
      (right_polarity != 1 && right_polarity != -1)) {
    return false;
  }
  capture_swr_ = NewResampler(48000, kCaptureChannels, 16000, kCaptureChannels);
  playback_swr_ = NewResampler(VoiceFrameContract::output_rate_hz, 1,
                               VoiceFrameContract::capture_rate_hz, 2);
  if (capture_swr_ == nullptr || playback_swr_ == nullptr) {
    Close();
    return false;
  }
  // 明确L=mono、R=mono，避免FFmpeg默认声道矩阵衰减。矩阵须在初始化前设置。
  swr_close(playback_swr_);
  const double stereo_matrix[] = {1.0, 1.0};
  if (swr_set_matrix(playback_swr_, stereo_matrix, 1) < 0 || swr_init(playback_swr_) < 0) {
    Close();
    return false;
  }
  left_polarity_ = left_polarity;
  right_polarity_ = right_polarity;
  return true;
}

bool AudioConverter::ResetCapture() noexcept {
  corrected48_.fill(0);
  return PrimeResampler(capture_swr_, corrected48_.data(), audio::kCaptureFrameSamples,
                        audio::kVoiceFrameSamples, interleaved16_.data());
}

bool AudioConverter::ResetPlayback() noexcept {
  if (playback_swr_ == nullptr) {
    return false;
  }
  swr_close(playback_swr_);
  playback_pending_frames_ = 0U;
  return swr_init(playback_swr_) >= 0;
}

bool AudioConverter::ConvertCapture(const audio::RawCaptureFrame& raw,
                                    audio::CaptureChannels* output) noexcept {
  if (capture_swr_ == nullptr || output == nullptr) {
    return false;
  }
  *output = {};
  corrected48_ = raw.pcm;
  // 只翻转物理麦克风；-32768 反相后先饱和，参考通道仍保留 Codec 原值。
  for (std::size_t i = 0U; i < audio::kCaptureFrameSamples; ++i) {
    const std::size_t base = kCaptureChannels * i;
    const int left = raw.pcm[base] * left_polarity_;
    const int right = raw.pcm[base + 1U] * right_polarity_;
    corrected48_[base] = static_cast<std::int16_t>(std::clamp(left, -32768, 32767));
    corrected48_[base + 1U] = static_cast<std::int16_t>(std::clamp(right, -32768, 32767));
  }
  // 四通道一次转换，不能各自创建重采样器，否则麦克风和参考的滤波相位可能不同。
  const std::uint8_t* in[] = {reinterpret_cast<const std::uint8_t*>(corrected48_.data())};
  std::uint8_t* out[] = {reinterpret_cast<std::uint8_t*>(interleaved16_.data())};
  const int converted = swr_convert(capture_swr_, out, audio::kVoiceFrameSamples, in,
                                    audio::kCaptureFrameSamples);
  if (converted != static_cast<int>(audio::kVoiceFrameSamples)) {
    return false;
  }
  for (std::size_t i = 0U; i < audio::kVoiceFrameSamples; ++i) {
    const std::size_t base = kCaptureChannels * i;
    output->mic_left[i] = interleaved16_[base];
    output->mic_right[i] = interleaved16_[base + 1U];
    output->reference_left[i] = interleaved16_[base + 2U];
  }
  output->metadata.timestamp_us = raw.timestamp_us;
  // AEC 前双麦取较大电平用于开口；AEC 后电平会由 SpeechDetector 另算，不能混用。
  output->metadata.input_dbfs =
      std::max(AcRmsDbfs(output->mic_left), AcRmsDbfs(output->mic_right));
  output->metadata.reference_active = std::any_of(
      output->reference_left.begin(), output->reference_left.end(), [](std::int16_t sample) {
        return sample > kReferencePeakThreshold || sample < -kReferencePeakThreshold;
      });
  return true;
}

bool AudioConverter::UpsamplePlayback(const std::int16_t* pcm, std::size_t samples,
                                      audio::StereoPlaybackFrame* output) noexcept {
  if (playback_swr_ == nullptr || (pcm == nullptr) != (samples == 0U) ||
      samples > audio::kTtsFrameSamples || output == nullptr) {
    return false;
  }
  output->frames = 0U;
  // FFmpeg的null flush可能吞掉不足滤波半窗的极短输入(真实库1样本回归)。
  // EOS送入只读静音来推进滤波，但输出上限仅为尚欠的有效采样时刻，不播放补齐静音。
  static constexpr std::array<std::int16_t, audio::kTtsFrameSamples> silence{};
  constexpr std::size_t ratio =
      VoiceFrameContract::capture_rate_hz / VoiceFrameContract::output_rate_hz;
  static_assert(VoiceFrameContract::capture_rate_hz % VoiceFrameContract::output_rate_hz == 0U,
                "playback duration requires an integer rate ratio");
  playback_pending_frames_ += samples * ratio;
  if (playback_pending_frames_ > audio::kPlaybackFrameCapacity) {
    return false;
  }
  if (playback_pending_frames_ == 0U) {
    return true;
  }
  const bool ending = pcm == nullptr;
  if (ending) {
    pcm = silence.data();
    samples = silence.size();
  }
  const std::uint8_t* in[] = {reinterpret_cast<const std::uint8_t*>(pcm)};
  std::uint8_t* out[] = {reinterpret_cast<std::uint8_t*>(output->pcm.data())};
  const int converted = swr_convert(
      playback_swr_, out,
      static_cast<int>(std::min(playback_pending_frames_, audio::kPlaybackFrameCapacity)), in,
      static_cast<int>(samples));
  if (converted < 0 || converted > static_cast<int>(audio::kPlaybackFrameCapacity) ||
      (ending && converted == 0)) {
    return false;
  }
  output->frames = static_cast<std::size_t>(converted);
  playback_pending_frames_ -= output->frames;
  return true;
}

long AudioConverter::Peak(const audio::StereoPlaybackFrame& frame) noexcept {
  if (frame.frames > audio::kPlaybackFrameCapacity) {
    return 0;
  }
  long peak = 0;
  for (std::size_t i = 0U; i < frame.frames * 2U; ++i) {
    peak = std::max(peak, std::abs(static_cast<long>(frame.pcm[i])));
  }
  return peak;
}

void AudioConverter::ApplyVolume(audio::StereoPlaybackFrame* frame, float gain,
                                 long peak) noexcept {
  if (frame == nullptr || frame->frames > audio::kPlaybackFrameCapacity) {
    return;
  }
  if (!std::isfinite(gain) || gain < 0.0F) {
    gain = 0.0F;
  }
  if (peak != 0 && gain * static_cast<float>(peak) > kPlaybackPeakLimit) {
    // 同一帧使用同一个受限增益，避免对峰顶逐样本硬剪切；后续整数饱和处理数值边界。
    gain = kPlaybackPeakLimit / static_cast<float>(peak);
  }
  for (std::size_t i = 0U; i < frame->frames * 2U; ++i) {
    const long value = std::lround(static_cast<float>(frame->pcm[i]) * gain);
    frame->pcm[i] = static_cast<std::int16_t>(
        std::clamp<long>(value, std::numeric_limits<std::int16_t>::min(),
                         std::numeric_limits<std::int16_t>::max()));
  }
}

void AudioConverter::Close() noexcept {
  swr_free(&capture_swr_);
  swr_free(&playback_swr_);
  playback_pending_frames_ = 0U;
  corrected48_.fill(0);
  interleaved16_.fill(0);
}

}  // namespace boompi::platform::rv1106
