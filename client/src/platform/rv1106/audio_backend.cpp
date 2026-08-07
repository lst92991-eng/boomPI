/// @file RV1106 板级音频后端：ALSA、硬件回采、重采样、Rockchip 3A、Snowboy 与 VAD。
#include "audio_backend.h"

#include <alsa/asoundlib.h>
extern "C" {
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
#include <webrtc_vad.h>
}
#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <mutex>
#include <new>

#include "boompi/platform/rv1106/rockchip_voice_dsp.h"
#include "snowboy_legacy_bridge.h"

namespace boompi::platform::rv1106 {
using audio::VoiceFrame16k;
using audio::kVoiceFrameSamples16k;
namespace {
using SteadyClock = std::chrono::steady_clock;
// Codec Mode1 每个 48 kHz frame 按 `[mic0,mic1,refL,refR]` 交错；播放为 L/R stereo。
constexpr std::size_t kCaptureChannels = 4U, kPlaybackChannels = 2U;
// 960@48 kHz 与 480@24 kHz 都是 20 ms，整个前端以该周期保持确定时序。
constexpr std::size_t kCapture48Frames = 960U, kTts24Frames = 480U;
// libswresample 可能因内部相位一次返回略多于严格 2 倍，静态数组预留两个 period。
constexpr std::size_t kMaximumPlayback48Frames = 2U * kCapture48Frames;
// 控制器最多提供 16 KiB DMA；四通道 S16 的两个 960-frame period 已占 15,360 字节。
// 因此连续性依赖专用实时采集线程及时排空，不能通过申请更大的硬件缓冲解决。
constexpr snd_pcm_uframes_t kCaptureBufferPeriods = 2U;
// 播放缓冲可容纳 80 ms，配合用户态 TTS ring 吸收短时调度和网络波动。
constexpr snd_pcm_uframes_t kPlaybackBufferPeriods = 4U;
constexpr std::uint32_t kFrameMs = 20U;
// 连续 120 ms 判定为开始，连续 700 ms 静音判定为结束，避免短噪声启动和句中停顿截断。
constexpr std::uint32_t kVadStartMs = 120U, kVadEndMs = 700U;
constexpr int kVadMode = 3;
// Mode1 参考首次出现后抑制 600 ms，正常播放结束后隔离 300 ms 扬声器与房间尾音。
constexpr unsigned kAecWarmupFrames = 30U;
constexpr unsigned kAecTailFrames = 15U;
// 参考任一 sample 绝对值超过 64 即视为活跃，只用于建立播放/AEC时序，不判断内容。
constexpr int kReferencePeakThreshold = 64;
constexpr const char* kLoopbackControl = "I2STDM Digital Loopback Mode";
constexpr const char* kLoopbackMode = "Mode1";

int OpenPcmHandle(const std::string& name, snd_pcm_stream_t stream,
                  snd_pcm_t** output) noexcept {
  // 禁止 ALSA plug 层静默改采样率、通道和格式；硬件契约不匹配时立即暴露错误。
  constexpr int flags = SND_PCM_NO_AUTO_RESAMPLE | SND_PCM_NO_AUTO_CHANNELS |
                        SND_PCM_NO_AUTO_FORMAT;
  return snd_pcm_open(output, name.c_str(), stream, flags);
}

int ConfigurePcm(snd_pcm_t* pcm, snd_pcm_stream_t stream,
                 unsigned channels, const char** stage) noexcept {
  // capture/playback 共用 48 kHz/S16_LE/20 ms period，只有通道数与缓冲 period 数不同。
  // 每一步更新 stage，使初始化失败日志能指向准确的 ALSA 协商阶段。
  snd_pcm_hw_params_t* hw = nullptr; snd_pcm_hw_params_alloca(&hw);
  unsigned rate = 48000U; snd_pcm_uframes_t period = kCapture48Frames;
  const snd_pcm_uframes_t buffer_periods =
      stream == SND_PCM_STREAM_CAPTURE ? kCaptureBufferPeriods
                                       : kPlaybackBufferPeriods;
  unsigned period_count = static_cast<unsigned>(buffer_periods);
  snd_pcm_uframes_t buffer = buffer_periods * kCapture48Frames; int direction = 0;
  *stage = "ALSA hw params any"; int rc = snd_pcm_hw_params_any(pcm, hw);
  if (rc >= 0) { *stage = "ALSA interleaved access"; rc = snd_pcm_hw_params_set_access(pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED); }
  if (rc >= 0) { *stage = "ALSA S16 format"; rc = snd_pcm_hw_params_set_format(pcm, hw, SND_PCM_FORMAT_S16_LE); }
  if (rc >= 0) { *stage = "ALSA channel count"; rc = snd_pcm_hw_params_set_channels(pcm, hw, channels); }
  if (rc >= 0) { *stage = "ALSA 48 kHz rate"; rc = snd_pcm_hw_params_set_rate(pcm, hw, rate, 0); }
  if (rc >= 0) { *stage = "ALSA period size"; rc = snd_pcm_hw_params_set_period_size_near(pcm, hw, &period, &direction); }
  direction = 0;
  if (rc >= 0) { *stage = "ALSA period count"; rc = snd_pcm_hw_params_set_periods_near(pcm, hw, &period_count, &direction); }
  if (rc >= 0) { *stage = "ALSA buffer size"; rc = snd_pcm_hw_params_set_buffer_size_near(pcm, hw, &buffer); }
  if (rc >= 0) { *stage = "ALSA apply hw params"; rc = snd_pcm_hw_params(pcm, hw); }
  if (rc < 0) return rc;
  if (period != kCapture48Frames || period_count != buffer_periods ||
      buffer != buffer_periods * kCapture48Frames) {
    // *_near 允许驱动协商相邻值；产品算法依赖严格 20 ms，协商结果必须再次验证。
    std::fprintf(stderr,
                 "boompi-client: ALSA negotiated period=%lu periods=%u buffer=%lu\n",
                 static_cast<unsigned long>(period),
                 period_count,
                 static_cast<unsigned long>(buffer));
    *stage = "ALSA exact period/buffer contract"; return -EINVAL;
  }
  snd_pcm_sw_params_t* sw = nullptr; snd_pcm_sw_params_alloca(&sw);
  *stage = "ALSA current software params"; rc = snd_pcm_sw_params_current(pcm, sw);
  if (rc >= 0) rc = snd_pcm_sw_params_set_avail_min(pcm, sw, kCapture48Frames);
  // capture 有一个 frame 即启动；playback 先在内核积累 60 ms，降低首播 XRUN 概率。
  const snd_pcm_uframes_t start = stream == SND_PCM_STREAM_CAPTURE ? 1U : 3U * kCapture48Frames;
  if (rc >= 0) rc = snd_pcm_sw_params_set_start_threshold(pcm, sw, start);
  if (rc >= 0) rc = snd_pcm_sw_params(pcm, sw);
  if (rc >= 0) { *stage = "ALSA PCM prepare"; rc = snd_pcm_prepare(pcm); }
  return rc;
}

void SetLoopbackId(snd_ctl_elem_id_t* id) noexcept {
  // 使用稳定的 mixer 控件名寻找回采模式，避免依赖不同镜像中的 numid。
  snd_ctl_elem_id_set_interface(id, SND_CTL_ELEM_IFACE_MIXER);
  snd_ctl_elem_id_set_name(id, kLoopbackControl);
}

int OpenLoopbackControl(int card, snd_ctl_t** control, snd_ctl_elem_id_t* id,
                        snd_ctl_elem_info_t* info) noexcept {
  // Mode1 必须是单值枚举控件；布局异常时拒绝继续，避免把未知通道送入 AEC。
  std::array<char, 24U> name{}; std::snprintf(name.data(), name.size(), "hw:%d", card);
  int rc = snd_ctl_open(control, name.data(), 0); if (rc < 0) return rc;
  SetLoopbackId(id); snd_ctl_elem_info_set_id(info, id); rc = snd_ctl_elem_info(*control, info);
  if (rc >= 0 && (snd_ctl_elem_info_get_type(info) != SND_CTL_ELEM_TYPE_ENUMERATED ||
                  snd_ctl_elem_info_get_count(info) != 1U)) rc = -EINVAL;
  if (rc < 0) { snd_ctl_close(*control); *control = nullptr; }
  return rc;
}

int ReadLoopbackValue(snd_ctl_t* control, snd_ctl_elem_id_t* id,
                      unsigned* output) noexcept {
  snd_ctl_elem_value_t* value = nullptr; snd_ctl_elem_value_alloca(&value);
  snd_ctl_elem_value_set_id(value, id); const int rc = snd_ctl_elem_read(control, value);
  if (rc >= 0) *output = snd_ctl_elem_value_get_enumerated(value, 0U);
  return rc;
}

int WriteLoopbackValue(snd_ctl_t* control, snd_ctl_elem_id_t* id,
                       unsigned target) noexcept {
  snd_ctl_elem_value_t* value = nullptr; snd_ctl_elem_value_alloca(&value);
  snd_ctl_elem_value_set_id(value, id); snd_ctl_elem_value_set_enumerated(value, 0U, target);
  int rc = snd_ctl_elem_write(control, value); unsigned actual = target;
  if (rc >= 0) rc = ReadLoopbackValue(control, id, &actual);
  return rc >= 0 && actual != target ? -EIO : rc;
}

int ConfigureLoopbackMode1(snd_pcm_t* capture) noexcept {
  // 先从已打开 PCM 查询真实声卡，再按枚举文本选择 Mode1，兼容枚举序号变化。
  snd_pcm_info_t* pcm_info = nullptr; snd_pcm_info_alloca(&pcm_info);
  int rc = snd_pcm_info(capture, pcm_info); if (rc < 0) return rc;
  const int card = snd_pcm_info_get_card(pcm_info);
  snd_ctl_t* control = nullptr; snd_ctl_elem_id_t* id = nullptr; snd_ctl_elem_info_t* info = nullptr;
  snd_ctl_elem_id_alloca(&id); snd_ctl_elem_info_alloca(&info);
  rc = OpenLoopbackControl(card, &control, id, info); if (rc < 0) return rc;
  const unsigned items = snd_ctl_elem_info_get_items(info); unsigned target = items;
  for (unsigned item = 0U; item < items && rc >= 0; ++item) {
    snd_ctl_elem_info_set_item(info, item); rc = snd_ctl_elem_info(control, info);
    if (rc >= 0 && std::strcmp(snd_ctl_elem_info_get_item_name(info), kLoopbackMode) == 0) target = item;
  }
  if (rc >= 0 && target == items) rc = -ENOENT;
  unsigned current = target;
  if (rc >= 0) rc = ReadLoopbackValue(control, id, &current);
  if (rc >= 0 && current != target) rc = WriteLoopbackValue(control, id, target);
  snd_ctl_close(control); return rc;
}

SwrContext* NewResampler(int in_rate, int in_channels, int out_rate,
                         int out_channels) noexcept {
  // capture 以四通道联合重采样，保证两路麦克风与参考始终共享同一滤波相位。
  SwrContext* swr = swr_alloc_set_opts(nullptr, av_get_default_channel_layout(out_channels),
      AV_SAMPLE_FMT_S16, out_rate, av_get_default_channel_layout(in_channels),
      AV_SAMPLE_FMT_S16, in_rate, 0, nullptr);
  if (swr == nullptr || swr_init(swr) < 0) { swr_free(&swr); return nullptr; }
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
  for (const std::int16_t sample : samples) sum += sample;
  const double mean = sum / static_cast<double>(samples.size());
  double energy = 0.0;
  for (const std::int16_t sample : samples) {
    const double centered = static_cast<double>(sample) - mean;
    energy += centered * centered;
  }
  if (energy <= 0.0) return -120.0F;
  const double rms = std::sqrt(energy / static_cast<double>(samples.size()));
  return static_cast<float>(20.0 * std::log10(rms / 32768.0));
}

std::uint64_t MonotonicUs() noexcept {
  // steady_clock 不受系统校时影响，适合测量连续 period 的真实间隔。
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(
          SteadyClock::now().time_since_epoch())
          .count());
}

}

struct AudioBackend::Impl final {
  // capture/DSP/Snowboy/VAD 归高优先级采集线程；playback 热路径归播放线程。
  AudioEngineConfig config{}; snd_pcm_t *capture_pcm{nullptr}, *playback_pcm{nullptr};
  // capture_swr: 48 kHz/4ch -> 16 kHz/4ch；playback_swr: 24 kHz/mono -> 48 kHz/mono。
  SwrContext *capture_swr{nullptr}, *playback_swr{nullptr}; RockchipVoiceDsp dsp{};
  BoompiSnowboyLegacyHandle* snowboy{nullptr}; VadInst* vad{nullptr};
  mutable std::mutex error_mutex{}; std::array<char, 192U> error{};
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
  bool delayed_reference_active{false};
  std::atomic<bool> capture_interrupted{false};
  std::atomic<bool> playback_render_started{false};
  std::atomic<bool> playback_interrupted{false};
  std::atomic<std::uint64_t> playback_xruns{0U};
  // playback 线程发布结束边沿，capture 线程在下一 period 消费并更新近讲门控。
  std::atomic<bool> playback_ended{false};

  void SetError(const char* text, int code = 0) noexcept {
    std::lock_guard<std::mutex> lock(error_mutex);
    if (code == 0) std::snprintf(error.data(), error.size(), "%s", text);
    else std::snprintf(error.data(), error.size(), "%s: %s", text, snd_strerror(code));
  }
  void ClearError() noexcept {
    std::lock_guard<std::mutex> lock(error_mutex);
    error.fill('\0');
  }
  bool ResetSwr(SwrContext* swr, const std::int16_t* input, int input_frames,
                int output_frames, std::int16_t* output) noexcept {
    // close/init 清除滤波器内部延迟，再送一帧静音建立确定初态；仅在控制边界调用。
    swr_close(swr); if (swr_init(swr) < 0) return false;
    const std::uint8_t* in[] = {reinterpret_cast<const std::uint8_t*>(input)};
    std::uint8_t* out[] = {reinterpret_cast<std::uint8_t*>(output)};
    return swr_convert(swr, out, output_frames, in, input_frames) >= 0;
  }
  bool ResetVad() noexcept {
    // WebRTC VAD 本身有跨帧历史，状态切换和 AEC 抑制窗结束时需要同时清零外部滞回。
    if (vad == nullptr || WebRtcVad_Init(vad) != 0 || WebRtcVad_set_mode(vad, kVadMode) != 0) return false;
    vad_speech_ms = vad_silence_ms = 0U; vad_in_speech = false; return true;
  }
  bool ResetListener() noexcept {
    return snowboy != nullptr && boompi_snowboy_legacy_reset(snowboy) && ResetVad();
  }
  bool ResetFrontEnd() noexcept {
    // discontinuity 后联合复位四通道重采样、3A FIFO、唤醒和 VAD 历史。
    capture48.fill(0);
    if (!ResetSwr(capture_swr, capture48.data(), kCapture48Frames,
                  kVoiceFrameSamples16k, capture16.data())) return false;
    delayed_input_dbfs = -120.0F;
    delayed_reference_active = false;
    delayed_timestamp_us = 0U;
    dsp.Close(); return dsp.Open() && ResetListener();
  }
  bool WritePlayback(const std::int16_t* mono, std::size_t frames) noexcept {
    // TTS 是 mono；复制到 L/R，Mode1 同步回采两个数字参考。
    for (std::size_t i = 0U; i < frames; ++i) {
      playback_stereo[2U * i] = mono[i]; playback_stereo[2U * i + 1U] = mono[i];
    }
    std::size_t offset = 0U;
    while (offset < frames) {
      // ALSA 允许部分写入；offset 保证每个 sample 只进入硬件时间线一次。
      if (playback_interrupted.load(std::memory_order_acquire)) return false;
      const snd_pcm_sframes_t rc = snd_pcm_writei(playback_pcm,
          playback_stereo.data() + 2U * offset, frames - offset);
      if (rc > 0) { offset += static_cast<std::size_t>(rc); continue; }
      if (playback_interrupted.load(std::memory_order_acquire)) return false;
      if (rc == -EINTR) continue;
      if (rc == -EPIPE || rc == -ESTRPIPE) {
        const int recovered = snd_pcm_recover(playback_pcm, static_cast<int>(rc), 1);
        if (recovered < 0) { SetError("ALSA playback recovery", recovered); return false; }
        playback_xruns.fetch_add(1U, std::memory_order_relaxed);
        // snd_pcm_writei 已接受的前缀已经进入同一条媒体时间线。恢复后只续写剩余
        // 样本；从零重写会在一次 20 ms 帧内制造可听见的重复前缀。
        std::fprintf(stderr,
                     "boompi-client: ALSA playback xrun recovered; error=%s; "
                     "accepted_frames=%zu\n",
                     snd_strerror(static_cast<int>(rc)), offset);
        continue;
      }
      SetError("ALSA playback write", static_cast<int>(rc)); return false;
    }
    return true;
  }
  bool UpdateVad(CaptureFrame* frame, float input_dbfs) noexcept {
    const int speech = WebRtcVad_Process(vad, 16000, processed.data(), processed.size());
    if (speech < 0) return false;
    // WebRTC VAD 判断频谱是否像语音；原始麦克风 dBFS 在 Rockchip 增益前做准入，
    // 避免远处人声和室内噪声因前端放大后直接启动一次 turn。
    frame->input_dbfs = input_dbfs;
    const bool webrtc_voice = speech == 1;
    // dBFS 只控制语句开始。进入说话态后由 WebRTC VAD 延续，低于门限的轻声尾字仍会保留。
    frame->vad_now = vad_in_speech
                         ? webrtc_voice
                         : webrtc_voice && input_dbfs >= config.vad_min_dbfs;
    if (frame->vad_now) {
      vad_speech_ms = std::min(vad_speech_ms + kFrameMs, kVadStartMs);
      vad_silence_ms = 0U;
    }
    else {
      vad_silence_ms = std::min(vad_silence_ms + kFrameMs, kVadEndMs);
      vad_speech_ms = 0U;
    }
    if (!vad_in_speech && vad_speech_ms >= kVadStartMs) {
      vad_in_speech = true;
      frame->vad_started = true;
    }
    else if (vad_in_speech && vad_silence_ms >= kVadEndMs) {
      vad_in_speech = false;
      frame->vad_ended = true;
    }
    return true;
  }

  bool UpdateNearVoice(CaptureFrame* frame) noexcept {
    // PreparePlayback 只武装预热；必须等 Mode1 真正出现参考，不能把 180 ms 首播缓存算进去。
    bool suppress = false;
    if (playback_ended.exchange(false, std::memory_order_acq_rel)) {
      // exchange 消费一次结束边沿，避免后续每帧重复启动尾音窗。
      playback_session_active = false;
      aec_warmup_armed = false;
      aec_warmup_remaining = 0U;
      // 打断发生前已经确认了 120 ms 近讲，必须保留其 VAD 生命周期直到 vad_ended。
      // 自然结束则隔离 300 ms 房间/扬声器尾音，结束时再清空这段抑制期的 VAD 历史。
      if (playback_interrupted.load(std::memory_order_acquire)) aec_tail_remaining = 0U;
      else { aec_tail_remaining = kAecTailFrames; suppress = true; }
    } else if (aec_warmup_armed) {
      suppress = true;
      if (!playback_render_started.load(std::memory_order_acquire)) {
        reference_wait_frames = 0U;
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
      if (--aec_warmup_remaining == 0U && !ResetVad()) return false;
    }
    if (aec_tail_remaining != 0U) {
      suppress = true;
      if (--aec_tail_remaining == 0U && !ResetVad()) return false;
    }
    frame->near_voice = !suppress && frame->vad_now;
    return true;
  }
};

AudioBackend::~AudioBackend() noexcept { Close(); delete impl_; }

bool AudioBackend::Open(const AudioEngineConfig& config) noexcept {
  if (impl_ == nullptr) impl_ = new (std::nothrow) Impl;
  if (impl_ == nullptr) return false;
  if (impl_->open) { impl_->SetError("audio backend is already open"); return false; }
  if (config.capture_pcm.empty() || config.playback_pcm.empty() || config.snowboy_resource.empty() ||
      config.snowboy_model.empty() || (config.left_polarity != 1 && config.left_polarity != -1) ||
      (config.right_polarity != 1 && config.right_polarity != -1) ||
      !std::isfinite(config.vad_min_dbfs) || config.vad_min_dbfs < -90.0F ||
      config.vad_min_dbfs > 0.0F) { impl_->SetError("invalid audio backend configuration"); return false; }
  try { impl_->config = config; } catch (...) { impl_->SetError("audio configuration allocation failed"); return false; }

  // 初始化顺序体现硬件依赖：先设置 Mode1 通道布局，再配置正式 PCM，最后创建算法对象。
  const char* stage = "ALSA capture open";
  int rc = OpenPcmHandle(config.capture_pcm, SND_PCM_STREAM_CAPTURE, &impl_->capture_pcm);
  // 驱动在 PCM open 时锁定通道布局：先用探测句柄取得 card，切 Mode1 后必须重新 open。
  if (rc >= 0) { stage = "ALSA Mode1 hardware reference";
                 rc = ConfigureLoopbackMode1(impl_->capture_pcm); }
  if (rc >= 0) {
    snd_pcm_close(impl_->capture_pcm); impl_->capture_pcm = nullptr;
    stage = "ALSA capture reopen after Mode1";
    rc = OpenPcmHandle(config.capture_pcm, SND_PCM_STREAM_CAPTURE, &impl_->capture_pcm);
  }
  if (rc >= 0) { stage = "ALSA capture exact 48k/4ch/960/1920 setup"; rc = ConfigurePcm(
      impl_->capture_pcm, SND_PCM_STREAM_CAPTURE, kCaptureChannels, &stage); }
  if (rc >= 0) { stage = "ALSA playback open"; rc = OpenPcmHandle(
      config.playback_pcm, SND_PCM_STREAM_PLAYBACK, &impl_->playback_pcm); }
  if (rc >= 0) { stage = "ALSA playback exact 48k/2ch/960/3840 setup"; rc = ConfigurePcm(
      impl_->playback_pcm, SND_PCM_STREAM_PLAYBACK, kPlaybackChannels, &stage); }
  if (rc < 0) { Close(); impl_->SetError(stage, rc); return false; }

  impl_->capture_swr = NewResampler(48000, kCaptureChannels, 16000, kCaptureChannels);
  impl_->playback_swr = NewResampler(24000, 1, 48000, 1);
  if (impl_->capture_swr == nullptr || impl_->playback_swr == nullptr ||
      boompi_snowboy_legacy_create(config.snowboy_resource.c_str(), config.snowboy_model.c_str(),
          config.snowboy_sensitivity.c_str(), 1.0F, &impl_->snowboy) == 0) {
    Close(); impl_->SetError("audio DSP, resampler, or Snowboy initialization failed"); return false;
  }
  impl_->vad = WebRtcVad_Create();
  if (impl_->vad == nullptr || WebRtcVad_Init(impl_->vad) != 0 || WebRtcVad_set_mode(impl_->vad, kVadMode) != 0 ||
      WebRtcVad_ValidRateAndFrameLength(16000, kVoiceFrameSamples16k) != 0 || !impl_->ResetFrontEnd()) {
    Close(); impl_->SetError("WebRTC VAD or voice front end initialization failed"); return false;
  }
  impl_->open = true;
  impl_->capture_interrupted.store(false, std::memory_order_release);
  impl_->playback_xruns.store(0U, std::memory_order_release);
  impl_->ClearError();
  std::fprintf(stderr,
               "boompi-client: VAD configured; mode=%d; min_dbfs=%.1f; source=raw-mic-max\n",
               kVadMode, static_cast<double>(config.vad_min_dbfs));
  return true;
}

bool AudioBackend::ReadCapture20ms(bool* const discontinuity) noexcept {
  if (impl_ == nullptr || !impl_->open || discontinuity == nullptr) return false;
  *discontinuity = false; std::size_t offset = 0U;
  while (offset < kCapture48Frames) {
    // readi 可能被信号打断或只返回部分 period；只有收齐 960 frame 才交给 DSP。
    const snd_pcm_sframes_t rc = snd_pcm_readi(impl_->capture_pcm,
        impl_->capture48.data() + kCaptureChannels * offset, kCapture48Frames - offset);
    if (rc > 0) { offset += static_cast<std::size_t>(rc); continue; }
    if (impl_->capture_interrupted.load(std::memory_order_acquire)) return false;
    if (rc == -EINTR) continue;
    if (rc == -EPIPE || rc == -ESTRPIPE) {
      const int recovered = snd_pcm_recover(impl_->capture_pcm, static_cast<int>(rc), 1);
      if (recovered < 0) { impl_->SetError("ALSA capture recovery", recovered); return false; }
      std::fprintf(stderr, "boompi-client: ALSA capture discontinuity recovered; error=%s\n",
                   snd_strerror(static_cast<int>(rc)));
      offset = 0U;
      *discontinuity = true;
      continue;
    }
    impl_->SetError("ALSA capture read", static_cast<int>(rc)); return false;
  }
  // 记录真实采集时序，避免 application 用 sequence 推算。发生调度抖动或
  // ALSA recover 时，上传时间戳因此仍会反映真实 period 间隔。
  impl_->capture_timestamp_us = MonotonicUs();
  return true;
}

bool AudioBackend::ProcessCapture20ms(bool discontinuity, CaptureFrame* const frame) noexcept {
  if (impl_ == nullptr || !impl_->open || frame == nullptr) return false;
  *frame = {};
  if (discontinuity) {
    // 时间轴断点会使重采样滤波、3A FIFO、Snowboy/VAD 历史全部失真，统一复位后
    // 交付显式断点帧，下一次调用再从完整的新 period 开始产生语音。
    frame->discontinuity = true;
    if (impl_->playback_session_active) {
      impl_->aec_warmup_armed = true;
      impl_->aec_warmup_remaining = 0U;
    }
    if (!impl_->ResetFrontEnd()) { impl_->SetError("voice front end reset failed"); return false; }
    return true;
  }
  // 极性只属于物理麦克风；数字回采必须保持 Codec 原样。
  for (std::size_t i = 0U; i < kCapture48Frames; ++i) {
    const std::size_t base = kCaptureChannels * i;
    const int left = impl_->capture48[base] * impl_->config.left_polarity;
    const int right = impl_->capture48[base + 1U] * impl_->config.right_polarity;
    impl_->capture48[base] = static_cast<std::int16_t>(std::clamp(
        left, static_cast<int>(std::numeric_limits<std::int16_t>::min()),
        static_cast<int>(std::numeric_limits<std::int16_t>::max())));
    impl_->capture48[base + 1U] = static_cast<std::int16_t>(std::clamp(
        right, static_cast<int>(std::numeric_limits<std::int16_t>::min()),
        static_cast<int>(std::numeric_limits<std::int16_t>::max())));
  }
  const std::uint8_t* in[] = {reinterpret_cast<const std::uint8_t*>(impl_->capture48.data())};
  std::uint8_t* out[] = {reinterpret_cast<std::uint8_t*>(impl_->capture16.data())};
  const int converted = swr_convert(impl_->capture_swr, out, kVoiceFrameSamples16k, in, kCapture48Frames);
  // 固定输入必须精确得到 320 frame；数量变化意味着媒体帧边界已经无法继续信任。
  if (converted != static_cast<int>(kVoiceFrameSamples16k)) { impl_->SetError("capture resampler lost frame alignment"); return false; }
  // 联合重采样结果仍按四通道交错，此处拆出两路麦克风和唯一左参考。
  for (std::size_t i = 0U; i < kVoiceFrameSamples16k; ++i) {
    const std::size_t base = kCaptureChannels * i;
    impl_->mic_left[i] = impl_->capture16[base];
    impl_->mic_right[i] = impl_->capture16[base + 1U];
    impl_->reference_left[i] = impl_->capture16[base + 2U];
#ifdef BOOMPI_AEC_LOOP_DIAGNOSTICS
    frame->aec_input[3][i] = impl_->capture16[base + 3U];
#endif
  }
  // 生产 AEC 固定为双麦单参考。Mode1 仍保留 REF-R 原始/HIL 数据，
  // 但打断门控必须和送入 Rockchip 3A 的 REF-L 使用同一证据。
  const bool current_reference_active = ReferenceActive(impl_->reference_left);
  const float current_input_dbfs = std::max(
      AcRmsDbfs(impl_->mic_left), AcRmsDbfs(impl_->mic_right));
#ifdef BOOMPI_AEC_LOOP_DIAGNOSTICS
  frame->aec_input[0] = impl_->mic_left;
  frame->aec_input[1] = impl_->mic_right;
  frame->aec_input[2] = impl_->reference_left;
#endif
  // 每个 sample 只进入这一条 Rockchip 3A 路径；STDT 在 vendor 内部保护双讲。
  if (!impl_->dsp.Process(impl_->mic_left, impl_->mic_right,
                          impl_->reference_left, &impl_->processed)) {
    impl_->SetError("Rockchip 3A rejected a frame");
    return false;
  }
  // 打断门限必须观察 AEC 后语音，不能使用必然包含扬声器声场的 raw mic dBFS。
  frame->voice_dbfs = AcRmsDbfs(impl_->processed);
  // vendor FIFO 的 mono 输出固定落后一帧；判定元数据必须取同一输入 period，
  // 不能把上一帧的 VAD 结果和当前帧的 dBFS/reference 拼在一起。
  frame->reference_active = impl_->delayed_reference_active;
  impl_->delayed_reference_active = current_reference_active;
  frame->timestamp_us = impl_->delayed_timestamp_us != 0U
                            ? impl_->delayed_timestamp_us
                            : impl_->capture_timestamp_us;
  impl_->delayed_timestamp_us = impl_->capture_timestamp_us;
  std::int32_t detection = 0;
  if (!boompi_snowboy_legacy_process_s16(impl_->snowboy, impl_->processed.data(),
                                         impl_->processed.size(), &detection)) {
    impl_->SetError("Snowboy processing failed");
    return false;
  }
  frame->wake = detection > 0;
  if (!impl_->UpdateVad(frame, impl_->delayed_input_dbfs)) {
    impl_->SetError("WebRTC VAD processing failed");
    return false;
  }
  impl_->delayed_input_dbfs = current_input_dbfs;
  // librkaudio 不公开 DTD 结果；近讲事件只能来自 AEC/STDT 后的真实输出。
  if (!impl_->UpdateNearVoice(frame)) {
    impl_->SetError("playback VAD reset failed");
    return false;
  }
  frame->pcm = impl_->processed;
  return true;
}

bool AudioBackend::ResetListener() noexcept {
  if (impl_ == nullptr || !impl_->open || !impl_->ResetListener()) {
    if (impl_ != nullptr) impl_->SetError("listener reset failed");
    return false;
  }
  return true;
}

bool AudioBackend::PreparePlayback() noexcept {
  if (impl_ == nullptr || !impl_->open) return false;
  // AudioEngine 只会让 capture 线程在帧边界调用本函数；返回后，
  // playback 线程才开始 Render/Drain 热路径，避免两线程同时改播放状态。
  const int prepared = snd_pcm_prepare(impl_->playback_pcm);
  if (prepared < 0) { impl_->SetError("ALSA playback prepare", prepared); return false; }
  impl_->playback24.fill(0); impl_->playback48.fill(0);
  if (!impl_->ResetSwr(impl_->playback_swr, impl_->playback24.data(), kTts24Frames,
                       impl_->playback48.size(), impl_->playback48.data())) {
    impl_->SetError("playback resampler reset failed"); return false;
  }
  impl_->playback_session_active = true;
  impl_->aec_warmup_armed = true;
  impl_->aec_warmup_remaining = 0U;
  impl_->reference_wait_frames = 0U;
  impl_->aec_tail_remaining = 0U;
  impl_->playback_ended.store(false, std::memory_order_release);
  impl_->playback_interrupted.store(false, std::memory_order_release);
  impl_->playback_render_started.store(false, std::memory_order_release);
  impl_->ClearError();
  return true;
}

bool AudioBackend::Render20ms(const std::int16_t* const pcm24, std::size_t samples, float gain) noexcept {
  if (impl_ == nullptr || !impl_->open || pcm24 == nullptr || samples > kTts24Frames || gain < 0.0F) return false;
  impl_->playback24.fill(0); std::copy_n(pcm24, samples, impl_->playback24.begin());
  // 最后一个短包补静音至完整 20 ms，让重采样器和 ALSA period 保持固定节拍。
  const std::uint8_t* in[] = {reinterpret_cast<const std::uint8_t*>(impl_->playback24.data())};
  std::uint8_t* out[] = {reinterpret_cast<std::uint8_t*>(impl_->playback48.data())};
  const int converted = swr_convert(impl_->playback_swr, out, impl_->playback48.size(), in, kTts24Frames);
  if (converted <= 0 || converted > static_cast<int>(impl_->playback48.size())) { impl_->SetError("TTS resampling failed"); return false; }
  long peak = 0;
  for (int i = 0; i < converted; ++i) peak = std::max(peak, std::abs(static_cast<long>(impl_->playback48[i])));
  if (peak != 0 && gain > 0.0F)
    impl_->playback_render_started.store(true, std::memory_order_release);
  // 95% 满幅为功放保留余量；扬声器非线性削顶无法由线性 AEC 消除。
  if (peak != 0 && gain * static_cast<float>(peak) > 31128.0F) gain = 31128.0F / static_cast<float>(peak);
  for (int i = 0; i < converted; ++i) {
    const long value = std::lround(static_cast<float>(impl_->playback48[i]) * gain);
    impl_->playback48[i] = static_cast<std::int16_t>(std::clamp<long>(value,
        std::numeric_limits<std::int16_t>::min(), std::numeric_limits<std::int16_t>::max()));
  }
  return impl_->WritePlayback(impl_->playback48.data(), static_cast<std::size_t>(converted));
}

bool AudioBackend::DrainPlayback() noexcept {
  if (impl_ == nullptr || !impl_->open) return false;
  // drain 保证自然回答的尾部已经离开内核缓冲；prepare 为下一会话恢复可写状态。
  const int drained = snd_pcm_drain(impl_->playback_pcm);
  if (drained < 0 && !impl_->playback_interrupted.load(std::memory_order_acquire)) impl_->SetError("ALSA playback drain", drained);
  const int prepared = snd_pcm_prepare(impl_->playback_pcm);
  if (prepared < 0 && !impl_->playback_interrupted.load(std::memory_order_acquire)) impl_->SetError("ALSA playback prepare", prepared);
  impl_->playback_ended.store(true, std::memory_order_release);
  impl_->playback_render_started.store(false, std::memory_order_release);
  return drained >= 0 && prepared >= 0;
}
void AudioBackend::DropPlayback() noexcept {
  if (impl_ == nullptr || impl_->playback_pcm == nullptr) return;
  // drop 是打断路径，允许舍弃内核中尚未出声的旧 TTS；随后立即 prepare 供下一轮使用。
  const int dropped = snd_pcm_drop(impl_->playback_pcm);
  const int prepared = snd_pcm_prepare(impl_->playback_pcm);
  if (dropped < 0 && dropped != -EBADFD) impl_->SetError("ALSA playback drop", dropped);
  if (prepared < 0) impl_->SetError("ALSA playback prepare", prepared);
  impl_->playback_ended.store(true, std::memory_order_release);
  impl_->playback_render_started.store(false, std::memory_order_release);
}
void AudioBackend::InterruptCapture() noexcept {
  if (impl_ == nullptr || impl_->capture_pcm == nullptr) return;
  impl_->capture_interrupted.store(true, std::memory_order_release);
  // abort 的职责是让 Close 中阻塞的 readi 返回，capture 线程会观察 stop 并退出。
  const int aborted = snd_pcm_abort(impl_->capture_pcm);
  if (aborted < 0) impl_->SetError("ALSA capture interrupt", aborted);
}
void AudioBackend::InterruptPlayback() noexcept {
  if (impl_ == nullptr || impl_->playback_pcm == nullptr) return;
  impl_->playback_interrupted.store(true, std::memory_order_release);
  // 先发布 interrupted 再 drop，WritePlayback 返回失败时上层可识别为主动取消。
  const int dropped = snd_pcm_drop(impl_->playback_pcm);
  if (dropped < 0 && dropped != -EBADFD) impl_->SetError("ALSA playback interrupt", dropped);
  impl_->playback_ended.store(true, std::memory_order_release);
  impl_->playback_render_started.store(false, std::memory_order_release);
}
std::uint64_t AudioBackend::playback_xruns() const noexcept {
  return impl_ == nullptr
      ? 0U
      : impl_->playback_xruns.load(std::memory_order_acquire);
}
std::string AudioBackend::last_error() const {
  if (impl_ == nullptr) return "audio backend is not allocated";
  std::lock_guard<std::mutex> lock(impl_->error_mutex); return impl_->error.data();
}
void AudioBackend::Close() noexcept {
  if (impl_ == nullptr) return;
  // boomPI 独占这块板的音频链路，Mode1 是运行状态；退出时无需切回会破坏通道布局的默认值。
  if (impl_->capture_pcm != nullptr) snd_pcm_close(impl_->capture_pcm);
  if (impl_->playback_pcm != nullptr) snd_pcm_close(impl_->playback_pcm);
  impl_->capture_pcm = nullptr;
  impl_->playback_pcm = nullptr;
  swr_free(&impl_->capture_swr); swr_free(&impl_->playback_swr); impl_->dsp.Close();
  if (impl_->snowboy != nullptr) boompi_snowboy_legacy_destroy(impl_->snowboy);
  if (impl_->vad != nullptr) WebRtcVad_Free(impl_->vad);
  impl_->snowboy = nullptr; impl_->vad = nullptr; impl_->open = false;
  impl_->playback_session_active = impl_->aec_warmup_armed = false;
  impl_->aec_warmup_remaining = impl_->aec_tail_remaining = 0U;
  impl_->reference_wait_frames = 0U;
  impl_->delayed_input_dbfs = -120.0F;
  impl_->delayed_reference_active = false;
  impl_->capture_interrupted.store(false, std::memory_order_release);
  impl_->playback_render_started.store(false, std::memory_order_release);
  impl_->playback_ended.store(false, std::memory_order_release);
  impl_->capture48.fill(0); impl_->capture16.fill(0); impl_->mic_left.fill(0); impl_->mic_right.fill(0);
  impl_->reference_left.fill(0); impl_->processed.fill(0);
}

}  // namespace boompi::platform::rv1106
