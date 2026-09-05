/// @file RV1106 板级音频后端：ALSA、硬件回采、重采样、Rockchip 3A、Snowboy 与 VAD。
#include "audio_backend.h"

#include "alsa_audio.h"
extern "C" {
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
#include <webrtc_vad.h>
}
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <limits>
#include <mutex>
#include <new>

#include "boompi/audio/board_voice_profile.h"
#include "boompi/platform/rv1106/rockchip_voice_dsp.h"
#include "snowboy_legacy_bridge.h"

namespace boompi::platform::rv1106 {
using audio::kBoardVoiceProfile;
using audio::kCaptureFrameSamples;
using audio::kTtsFrameSamples;
using audio::kVoiceFrameSamples16k;
using audio::VoiceFrame16k;
using audio::VoiceFrameContract;
namespace {
using SteadyClock = std::chrono::steady_clock;
// Codec Mode1 每个 48 kHz frame 按 `[mic0,mic1,refL,refR]` 交错；播放为 L/R stereo。
constexpr std::size_t kCaptureChannels = VoiceFrameContract::capture_channels;
constexpr std::size_t kPlaybackChannels = VoiceFrameContract::playback_channels;
// 960@48 kHz 与 480@24 kHz 都是 20 ms，整个前端以该周期保持确定时序。
constexpr std::size_t kCapture48Frames = kCaptureFrameSamples;
constexpr std::size_t kTts24Frames = kTtsFrameSamples;
// libswresample 可能因内部相位一次返回略多于严格 2 倍，静态数组预留两个 period。
constexpr std::size_t kMaximumPlayback48Frames = 2U * kCapture48Frames;
constexpr std::uint32_t kFrameMs = VoiceFrameContract::frame_ms;
// 连续 120 ms 判定为开始，连续 700 ms 静音判定为结束，避免短噪声启动和句中停顿截断。
constexpr std::uint32_t kVadStartMs = 120U, kVadEndMs = 700U;
constexpr int kVadMode = 3;
// Mode1 参考首次出现后抑制 600 ms，正常播放结束后隔离 300 ms 扬声器与房间尾音。
constexpr unsigned kAecWarmupFrames =
    static_cast<unsigned>(VoiceFrameContract::FramesForMs(600U));
constexpr unsigned kAecTailFrames =
    static_cast<unsigned>(VoiceFrameContract::FramesForMs(300U));
// 参考任一 sample 绝对值超过 64 即视为活跃，只用于建立播放/AEC时序，不判断内容。
constexpr int kReferencePeakThreshold = 64;
SwrContext* NewResampler(int in_rate, int in_channels, int out_rate,
                         int out_channels) noexcept {
  // capture 以四通道联合重采样，保证两路麦克风与参考始终共享同一滤波相位。
  SwrContext* swr = swr_alloc_set_opts(
      nullptr, av_get_default_channel_layout(out_channels), AV_SAMPLE_FMT_S16, out_rate,
      av_get_default_channel_layout(in_channels), AV_SAMPLE_FMT_S16, in_rate, 0, nullptr);
  if (swr == nullptr || swr_init(swr) < 0) {
    swr_free(&swr);
    return nullptr;
  }
  return swr;
}

bool ReferenceActive(const VoiceFrame16k& reference) noexcept {
  return std::any_of(reference.begin(), reference.end(), [](std::int16_t sample) {
    return sample > kReferencePeakThreshold || sample < -kReferencePeakThreshold;
  });
}

float AcRmsDbfs(const VoiceFrame16k& samples) noexcept {
  // 先移除直流分量再算 RMS，麦克风偏置不会抬高 VAD 准入电平。
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

std::uint64_t MonotonicUs() noexcept {
  // steady_clock 不受系统校时影响，适合测量连续 period 的真实间隔。
  return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                        SteadyClock::now().time_since_epoch())
                                        .count());
}

}  // namespace

struct AudioBackend::Impl final {
  // capture/DSP/Snowboy/VAD 归高优先级采集线程；playback 热路径归播放线程。
  AudioEngineConfig config{};
  AlsaAudio pcm{};
  // capture_swr: 48 kHz/4ch -> 16 kHz/4ch；playback_swr: 24 kHz/mono -> 48 kHz/mono。
  SwrContext* capture_swr{nullptr};
  SwrContext* playback_swr{nullptr};
  RockchipVoiceDsp dsp{};
  BoompiSnowboyLegacyHandle* snowboy{nullptr};
  VadInst* vad{nullptr};
  mutable std::mutex error_mutex{};
  std::array<char, 192U> error{};
  // 所有音频缓冲都在 Open 前随 Impl 分配，实时路径不申请堆内存。
  std::array<std::int16_t, kCapture48Frames * kCaptureChannels> capture48{};
  std::array<std::int16_t, kVoiceFrameSamples16k * kCaptureChannels> capture16{};
  std::array<std::int16_t, kTts24Frames> playback24{};
  std::array<std::int16_t, kMaximumPlayback48Frames> playback48{};
  std::array<std::int16_t, kMaximumPlayback48Frames * kPlaybackChannels> playback_stereo{};
  VoiceFrame16k mic_left{}, mic_right{}, reference_left{}, processed{};
  // VAD 使用毫秒累计器形成开始/结束滞回；计数只由 capture 线程更新。
  std::uint32_t vad_speech_ms{0U}, vad_silence_ms{0U};
  // warmup/tail 把 AEC 收敛期和播放尾音与可打断近讲事件隔离。
  unsigned aec_warmup_remaining{0U}, aec_tail_remaining{0U};
  unsigned reference_wait_frames{0U};
  float delayed_input_dbfs{-120.0F};
  std::uint64_t capture_timestamp_us{0U};
  // librkaudio FIFO 固定产生一帧延迟，原始电平、参考状态和时间戳同步延迟一帧。
  std::uint64_t delayed_timestamp_us{0U};
  bool open{false}, vad_in_speech{false};
  bool playback_session_active{false}, aec_warmup_armed{false};
  bool silent_playback_bypass{false};
  bool delayed_reference_active{false};
  std::atomic<bool> playback_render_started{false};
  std::atomic<bool> playback_output_audible{false};
  // playback 线程发布结束边沿，capture 线程在下一 period 消费并更新近讲门控。
  std::atomic<bool> playback_ended{false};

  void SetError(const char* text) noexcept {
    std::lock_guard<std::mutex> lock(error_mutex);
    std::snprintf(error.data(), error.size(), "%s", text);
  }
  void ClearError() noexcept {
    std::lock_guard<std::mutex> lock(error_mutex);
    error.fill('\0');
  }
  bool ResetSwr(SwrContext* swr, const std::int16_t* input, int input_frames, int output_frames,
                std::int16_t* output) noexcept {
    // close/init 清除滤波器内部延迟，再送一帧静音建立确定初态；仅在控制边界调用。
    swr_close(swr);
    if (swr_init(swr) < 0) {
      return false;
    }
    const std::uint8_t* in[] = {reinterpret_cast<const std::uint8_t*>(input)};
    std::uint8_t* out[] = {reinterpret_cast<std::uint8_t*>(output)};
    return swr_convert(swr, out, output_frames, in, input_frames) >= 0;
  }
  bool ResetVad() noexcept {
    // WebRTC VAD 本身有跨帧历史，状态切换和 AEC 抑制窗结束时需要同时清零外部滞回。
    if (vad == nullptr || WebRtcVad_Init(vad) != 0 || WebRtcVad_set_mode(vad, kVadMode) != 0) {
      return false;
    }
    vad_speech_ms = vad_silence_ms = 0U;
    vad_in_speech = false;
    return true;
  }
  bool ResetListener() noexcept {
    return snowboy != nullptr && boompi_snowboy_legacy_reset(snowboy) && ResetVad();
  }
  bool ResetFrontEnd() noexcept {
    // discontinuity 后联合复位四通道重采样、3A FIFO、唤醒和 VAD 历史。
    capture48.fill(0);
    if (!ResetSwr(capture_swr, capture48.data(), kCapture48Frames, kVoiceFrameSamples16k,
                  capture16.data())) {
      return false;
    }
    delayed_input_dbfs = -120.0F;
    delayed_reference_active = false;
    delayed_timestamp_us = 0U;
    dsp.Close();
    return dsp.Open(kBoardVoiceProfile.aec_delay_samples) && ResetListener();
  }
  bool WritePlayback(const std::int16_t* mono, std::size_t frames) noexcept {
    // TTS 是 mono；复制到 L/R，Mode1 同步回采两个数字参考。
    for (std::size_t i = 0U; i < frames; ++i) {
      playback_stereo[2U * i] = mono[i];
      playback_stereo[2U * i + 1U] = mono[i];
    }
    return pcm.WritePlayback(playback_stereo.data(), frames);
  }
  bool UpdateVad(CaptureFrame* frame, float input_dbfs) noexcept {
    const int speech = WebRtcVad_Process(vad, 16000, processed.data(), processed.size());
    if (speech < 0) {
      return false;
    }
    // WebRTC VAD 判断频谱是否像语音；原始麦克风 dBFS 在 Rockchip 增益前做准入，
    // 避免远处人声和室内噪声因前端放大后直接启动一次 turn。
    frame->input_dbfs = input_dbfs;
    const bool webrtc_voice = speech == 1;
    // dBFS 只控制语句开始。进入说话态后由 WebRTC VAD 延续，低于门限的轻声尾字仍会保留。
    const bool admitted = vad_in_speech || input_dbfs >= config.vad_min_dbfs;
    frame->vad_now = webrtc_voice && admitted;
    if (frame->vad_now) {
      vad_speech_ms = std::min(vad_speech_ms + kFrameMs, kVadStartMs);
      vad_silence_ms = 0U;
    } else {
      vad_silence_ms = std::min(vad_silence_ms + kFrameMs, kVadEndMs);
      vad_speech_ms = 0U;
    }
    if (!vad_in_speech && vad_speech_ms >= kVadStartMs) {
      vad_in_speech = true;
      frame->vad_started = true;
    } else if (vad_in_speech && vad_silence_ms >= kVadEndMs) {
      vad_in_speech = false;
      frame->vad_ended = true;
    }
    return true;
  }

  bool UpdateNearVoice(CaptureFrame* frame) noexcept {
    // ArmPlayback 只武装预热；必须等 Mode1 真正出现参考，不能把 180 ms 首播缓存算进去。
    bool suppress = false;
    if (playback_ended.exchange(false, std::memory_order_acq_rel)) {
      // exchange 消费一次结束边沿，避免后续每帧重复启动尾音窗。
      playback_session_active = false;
      aec_warmup_armed = false;
      silent_playback_bypass = false;
      aec_warmup_remaining = 0U;
      // 打断发生前已经确认了 120 ms 近讲，必须保留其 VAD 生命周期直到 vad_ended。
      // 自然结束则隔离 300 ms 房间/扬声器尾音，结束时再清空这段抑制期的 VAD 历史。
      if (pcm.playback_interrupted()) {
        aec_tail_remaining = 0U;
      } else {
        aec_tail_remaining = kAecTailFrames;
        suppress = true;
      }
    } else if (silent_playback_bypass &&
               playback_output_audible.load(std::memory_order_acquire)) {
      // 用户在静音播放中调高音量后，重新等待真实 Mode1 reference 并完成预热。
      silent_playback_bypass = false;
      aec_warmup_armed = true;
      reference_wait_frames = 0U;
      suppress = true;
    } else if (aec_warmup_armed) {
      suppress = true;
      if (!playback_render_started.load(std::memory_order_acquire)) {
        reference_wait_frames = 0U;
      } else if (!playback_output_audible.load(std::memory_order_acquire)) {
        // 音量为零时扬声器没有回声源。继续永久等待 reference 会把本应可用的
        // 近讲入口一并锁死；先旁路预热，之后一旦重新出声再重新武装。
        aec_warmup_armed = false;
        silent_playback_bypass = true;
        reference_wait_frames = 0U;
        suppress = false;
      } else if (frame->reference_active) {
        aec_warmup_armed = false;
        aec_warmup_remaining = kAecWarmupFrames;
        reference_wait_frames = 0U;
      } else if (++reference_wait_frames == 50U) {
        // 播放已提交但 1 秒没有硬件参考，提示检查 Mode1 通道或播放链路。
        std::fprintf(stderr,
                     "boompi-client: warning: AEC reference absent for 1000 ms of playback\n");
      }
    }
    if (aec_warmup_remaining != 0U) {
      suppress = true;
      if (--aec_warmup_remaining == 0U && !ResetVad()) {
        return false;
      }
    }
    if (aec_tail_remaining != 0U) {
      suppress = true;
      if (--aec_tail_remaining == 0U && !ResetVad()) {
        return false;
      }
    }
    frame->near_voice = !suppress && frame->vad_now;
    return true;
  }

  void ApplyMicrophonePolarity() noexcept {
    // 反相只处理物理麦克风，参考保留 Codec 原值。-32768 反相后需饱和到 32767。
    for (std::size_t i = 0U; i < kCapture48Frames; ++i) {
      const std::size_t base = kCaptureChannels * i;
      const int left = capture48[base] * config.left_polarity;
      const int right = capture48[base + 1U] * config.right_polarity;
      capture48[base] = static_cast<std::int16_t>(
          std::clamp(left, static_cast<int>(std::numeric_limits<std::int16_t>::min()),
                     static_cast<int>(std::numeric_limits<std::int16_t>::max())));
      capture48[base + 1U] = static_cast<std::int16_t>(
          std::clamp(right, static_cast<int>(std::numeric_limits<std::int16_t>::min()),
                     static_cast<int>(std::numeric_limits<std::int16_t>::max())));
    }
  }

  bool ResampleCapture() noexcept {
    // 四通道一起重采样，麦克风与参考共享滤波相位；随后拆出 3A 需要的三个平面。
    const std::uint8_t* in[] = {reinterpret_cast<const std::uint8_t*>(capture48.data())};
    std::uint8_t* out[] = {reinterpret_cast<std::uint8_t*>(capture16.data())};
    const int converted =
        swr_convert(capture_swr, out, kVoiceFrameSamples16k, in, kCapture48Frames);
    if (converted != static_cast<int>(kVoiceFrameSamples16k)) {
      SetError("capture resampler lost frame alignment");
      return false;
    }
    for (std::size_t i = 0U; i < kVoiceFrameSamples16k; ++i) {
      const std::size_t base = kCaptureChannels * i;
      mic_left[i] = capture16[base];
      mic_right[i] = capture16[base + 1U];
      reference_left[i] = capture16[base + 2U];
    }
    return true;
  }

  bool AnalyzeCapture(CaptureFrame* frame) noexcept {
    // 3A 的 mono 输出固定晚一帧；电平、参考和时间戳也延迟一帧，
    // 避免用本帧参考去判断上一帧残留的扬声器声音。
    frame->voice_dbfs = AcRmsDbfs(processed);
    frame->reference_active = delayed_reference_active;
    delayed_reference_active = ReferenceActive(reference_left);
    frame->timestamp_us = delayed_timestamp_us;
    if (frame->timestamp_us == 0U) {
      frame->timestamp_us = capture_timestamp_us;
    }
    delayed_timestamp_us = capture_timestamp_us;

    std::int32_t detection = 0;
    if (!boompi_snowboy_legacy_process_s16(snowboy, processed.data(), processed.size(),
                                           &detection)) {
      SetError("Snowboy processing failed");
      return false;
    }
    frame->wake = detection > 0;
    if (!UpdateVad(frame, delayed_input_dbfs)) {
      SetError("WebRTC VAD processing failed");
      return false;
    }
    delayed_input_dbfs = std::max(AcRmsDbfs(mic_left), AcRmsDbfs(mic_right));
    // vendor 不公开双讲检测结果，近讲判断使用 3A 输出及播放预热/尾音状态。
    if (!UpdateNearVoice(frame)) {
      SetError("playback VAD reset failed");
      return false;
    }
    frame->pcm = processed;
    return true;
  }

  void PublishPlaybackEnd() noexcept {
    playback_ended.store(true, std::memory_order_release);
    playback_render_started.store(false, std::memory_order_release);
    playback_output_audible.store(false, std::memory_order_release);
  }
};

AudioBackend::~AudioBackend() noexcept {
  Close();
  delete impl_;
}

bool AudioBackend::Open(const AudioEngineConfig& config) noexcept {
  if (impl_ == nullptr) {
    impl_ = new (std::nothrow) Impl;
  }
  if (impl_ == nullptr) {
    return false;
  }
  if (impl_->open) {
    impl_->SetError("audio backend is already open");
    return false;
  }
  const bool missing_resource = config.capture_pcm.empty() || config.playback_pcm.empty() ||
                                config.snowboy_resource.empty() || config.snowboy_model.empty();
  const bool invalid_polarity = (config.left_polarity != 1 && config.left_polarity != -1) ||
                                (config.right_polarity != 1 && config.right_polarity != -1);
  const bool invalid_vad = !std::isfinite(config.vad_min_dbfs) ||
                           config.vad_min_dbfs < -90.0F || config.vad_min_dbfs > 0.0F;
  if (missing_resource || invalid_polarity || invalid_vad) {
    impl_->SetError("invalid audio backend configuration");
    return false;
  }
  try {
    impl_->config = config;
  } catch (...) {
    impl_->SetError("audio configuration allocation failed");
    return false;
  }

  impl_->ClearError();
  if (!impl_->pcm.Open(config.capture_pcm, config.playback_pcm)) {
    return false;
  }

  impl_->capture_swr = NewResampler(48000, kCaptureChannels, 16000, kCaptureChannels);
  impl_->playback_swr = NewResampler(24000, 1, 48000, 1);
  if (impl_->capture_swr == nullptr || impl_->playback_swr == nullptr ||
      boompi_snowboy_legacy_create(
          config.snowboy_resource.c_str(), config.snowboy_model.c_str(),
          config.snowboy_sensitivity.c_str(), 1.0F, &impl_->snowboy) == 0) {
    Close();
    impl_->SetError("audio DSP, resampler, or Snowboy initialization failed");
    return false;
  }
  impl_->vad = WebRtcVad_Create();
  if (impl_->vad == nullptr || WebRtcVad_Init(impl_->vad) != 0 ||
      WebRtcVad_set_mode(impl_->vad, kVadMode) != 0 ||
      WebRtcVad_ValidRateAndFrameLength(16000, kVoiceFrameSamples16k) != 0 ||
      !impl_->ResetFrontEnd()) {
    Close();
    impl_->SetError("WebRTC VAD or voice front end initialization failed");
    return false;
  }
  impl_->open = true;
  impl_->ClearError();
  std::fprintf(stderr,
               "boompi-client: VAD configured; mode=%d; min_dbfs=%.1f; source=raw-mic-max\n",
               kVadMode, static_cast<double>(config.vad_min_dbfs));
  return true;
}

bool AudioBackend::ReadCapture20ms(bool* const discontinuity) noexcept {
  if (impl_ == nullptr || !impl_->open) {
    return false;
  }
  if (!impl_->pcm.ReadCapture20ms(impl_->capture48.data(), discontinuity)) {
    return false;
  }
  // 记录真实采集时序，避免 application 用 sequence 推算。发生调度抖动或
  // ALSA recover 时，上传时间戳因此仍会反映真实 period 间隔。
  impl_->capture_timestamp_us = MonotonicUs();
  return true;
}

bool AudioBackend::ProcessCapture20ms(bool discontinuity, CaptureFrame* const frame) noexcept {
  if (impl_ == nullptr || !impl_->open || frame == nullptr) {
    return false;
  }
  *frame = {};
  if (discontinuity) {
    // 时间轴断点会使重采样滤波、3A FIFO、Snowboy/VAD 历史全部失真，统一复位后
    // 交付显式断点帧，下一次调用再从完整的新 period 开始产生语音。
    frame->discontinuity = true;
    if (impl_->playback_session_active) {
      impl_->aec_warmup_armed = true;
      impl_->aec_warmup_remaining = 0U;
    }
    if (!impl_->ResetFrontEnd()) {
      impl_->SetError("voice front end reset failed");
      return false;
    }
    return true;
  }
  impl_->ApplyMicrophonePolarity();
  if (!impl_->ResampleCapture()) {
    return false;
  }
  if (!impl_->dsp.Process(impl_->mic_left, impl_->mic_right, impl_->reference_left,
                          &impl_->processed)) {
    impl_->SetError("Rockchip 3A rejected a frame");
    return false;
  }
  return impl_->AnalyzeCapture(frame);
}

bool AudioBackend::ResetListener() noexcept {
  if (impl_ == nullptr || !impl_->open || !impl_->ResetListener()) {
    if (impl_ != nullptr) {
      impl_->SetError("listener reset failed");
    }
    return false;
  }
  return true;
}

bool AudioBackend::ArmPlayback() noexcept {
  if (impl_ == nullptr || !impl_->open) {
    return false;
  }
  // 采集帧边界先武装 AEC；真正听到 Mode1 参考后才开始预热计时。
  impl_->playback_session_active = true;
  impl_->aec_warmup_armed = true;
  impl_->silent_playback_bypass = false;
  impl_->aec_warmup_remaining = 0U;
  impl_->reference_wait_frames = 0U;
  impl_->aec_tail_remaining = 0U;
  impl_->playback_ended.store(false, std::memory_order_release);
  impl_->playback_render_started.store(false, std::memory_order_release);
  impl_->playback_output_audible.store(false, std::memory_order_release);
  impl_->pcm.ClearError();
  impl_->ClearError();
  return true;
}

bool AudioBackend::PreparePlayback() noexcept {
  if (impl_ == nullptr || !impl_->open || !impl_->pcm.PreparePlayback()) {
    return false;
  }
  // 播放线程独占 PCM 和播放重采样器；AudioEngine 在状态锁内准备，防止越过用户 drop。
  impl_->playback24.fill(0);
  impl_->playback48.fill(0);
  if (!impl_->ResetSwr(impl_->playback_swr, impl_->playback24.data(), kTts24Frames,
                       impl_->playback48.size(), impl_->playback48.data())) {
    impl_->SetError("playback resampler reset failed");
    return false;
  }
  return true;
}

bool AudioBackend::Render20ms(const std::int16_t* const pcm24, std::size_t samples,
                              float gain) noexcept {
  if (impl_ == nullptr || !impl_->open || pcm24 == nullptr || samples > kTts24Frames ||
      gain < 0.0F) {
    return false;
  }
  impl_->playback24.fill(0);
  std::copy_n(pcm24, samples, impl_->playback24.begin());
  // 最后一个短包补静音至完整 20 ms，让重采样器和 ALSA period 保持固定节拍。
  const std::uint8_t* in[] = {reinterpret_cast<const std::uint8_t*>(impl_->playback24.data())};
  std::uint8_t* out[] = {reinterpret_cast<std::uint8_t*>(impl_->playback48.data())};
  const int converted =
      swr_convert(impl_->playback_swr, out, impl_->playback48.size(), in, kTts24Frames);
  if (converted <= 0 || converted > static_cast<int>(impl_->playback48.size())) {
    impl_->SetError("TTS resampling failed");
    return false;
  }
  long peak = 0;
  for (int i = 0; i < converted; ++i) {
    peak = std::max(peak, std::abs(static_cast<long>(impl_->playback48[i])));
  }
  if (peak != 0) {
    impl_->playback_render_started.store(true, std::memory_order_release);
    impl_->playback_output_audible.store(gain > 0.0F, std::memory_order_release);
  }
  // 95% 满幅为功放保留余量；扬声器非线性削顶无法由线性 AEC 消除。
  if (peak != 0 && gain * static_cast<float>(peak) > 31128.0F) {
    gain = 31128.0F / static_cast<float>(peak);
  }
  for (int i = 0; i < converted; ++i) {
    const long value = std::lround(static_cast<float>(impl_->playback48[i]) * gain);
    impl_->playback48[i] = static_cast<std::int16_t>(
        std::clamp<long>(value, std::numeric_limits<std::int16_t>::min(),
                         std::numeric_limits<std::int16_t>::max()));
  }
  return impl_->WritePlayback(impl_->playback48.data(), static_cast<std::size_t>(converted));
}

bool AudioBackend::DrainPlayback() noexcept {
  if (impl_ == nullptr || !impl_->open) {
    return false;
  }
  const bool drained = impl_->pcm.DrainPlayback();
  impl_->PublishPlaybackEnd();
  return drained;
}
void AudioBackend::DropPlayback() noexcept {
  if (impl_ == nullptr) {
    return;
  }
  impl_->pcm.DropPlayback();
  impl_->PublishPlaybackEnd();
}
void AudioBackend::InterruptCapture() noexcept {
  if (impl_ != nullptr) {
    impl_->pcm.InterruptCapture();
  }
}
void AudioBackend::InterruptPlayback() noexcept {
  if (impl_ == nullptr) {
    return;
  }
  impl_->pcm.InterruptPlayback();
  impl_->PublishPlaybackEnd();
}
std::uint64_t AudioBackend::playback_xruns() const noexcept {
  return impl_ == nullptr ? 0U : impl_->pcm.playback_xruns();
}
std::string AudioBackend::last_error() const {
  if (impl_ == nullptr) {
    return "audio backend is not allocated";
  }
  std::lock_guard<std::mutex> lock(impl_->error_mutex);
  if (impl_->error[0] != '\0') {
    return impl_->error.data();
  }
  return impl_->pcm.last_error();
}
void AudioBackend::Close() noexcept {
  if (impl_ == nullptr) {
    return;
  }
  impl_->pcm.Close();
  swr_free(&impl_->capture_swr);
  swr_free(&impl_->playback_swr);
  impl_->dsp.Close();
  if (impl_->snowboy != nullptr) {
    boompi_snowboy_legacy_destroy(impl_->snowboy);
  }
  if (impl_->vad != nullptr) {
    WebRtcVad_Free(impl_->vad);
  }
  impl_->snowboy = nullptr;
  impl_->vad = nullptr;
  impl_->open = false;
  impl_->playback_session_active = impl_->aec_warmup_armed = impl_->silent_playback_bypass =
      false;
  impl_->aec_warmup_remaining = impl_->aec_tail_remaining = 0U;
  impl_->reference_wait_frames = 0U;
  impl_->delayed_input_dbfs = -120.0F;
  impl_->delayed_reference_active = false;
  impl_->playback_render_started.store(false, std::memory_order_release);
  impl_->playback_output_audible.store(false, std::memory_order_release);
  impl_->playback_ended.store(false, std::memory_order_release);
  impl_->capture48.fill(0);
  impl_->capture16.fill(0);
  impl_->mic_left.fill(0);
  impl_->mic_right.fill(0);
  impl_->reference_left.fill(0);
  impl_->processed.fill(0);
}

}  // namespace boompi::platform::rv1106
