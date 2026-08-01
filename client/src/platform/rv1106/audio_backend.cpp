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
constexpr std::size_t kCaptureChannels = 4U, kPlaybackChannels = 2U;
constexpr std::size_t kCapture48Frames = 960U, kTts24Frames = 480U;
constexpr std::size_t kMaximumPlayback48Frames = 2U * kCapture48Frames;
constexpr std::uint32_t kFrameMs = 20U;
constexpr unsigned kAecWarmupFrames = 30U;
constexpr unsigned kAecTailFrames = 15U;
constexpr int kReferencePeakThreshold = 64;
constexpr const char* kLoopbackControl = "I2STDM Digital Loopback Mode";
constexpr const char* kLoopbackMode = "Mode1";

int OpenPcmHandle(const std::string& name, snd_pcm_stream_t stream,
                  snd_pcm_t** output) noexcept {
  constexpr int flags = SND_PCM_NO_AUTO_RESAMPLE | SND_PCM_NO_AUTO_CHANNELS |
                        SND_PCM_NO_AUTO_FORMAT;
  return snd_pcm_open(output, name.c_str(), stream, flags);
}

int ConfigurePcm(snd_pcm_t* pcm, snd_pcm_stream_t stream,
                 unsigned channels, const char** stage) noexcept {
  snd_pcm_hw_params_t* hw = nullptr; snd_pcm_hw_params_alloca(&hw);
  unsigned rate = 48000U; snd_pcm_uframes_t period = kCapture48Frames;
  const snd_pcm_uframes_t buffer_periods =
      stream == SND_PCM_STREAM_CAPTURE ? 2U : 4U;
  snd_pcm_uframes_t buffer = buffer_periods * kCapture48Frames; int direction = 0;
  *stage = "ALSA hw params any"; int rc = snd_pcm_hw_params_any(pcm, hw);
  if (rc >= 0) { *stage = "ALSA interleaved access"; rc = snd_pcm_hw_params_set_access(pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED); }
  if (rc >= 0) { *stage = "ALSA S16 format"; rc = snd_pcm_hw_params_set_format(pcm, hw, SND_PCM_FORMAT_S16_LE); }
  if (rc >= 0) { *stage = "ALSA channel count"; rc = snd_pcm_hw_params_set_channels(pcm, hw, channels); }
  if (rc >= 0) { *stage = "ALSA 48 kHz rate"; rc = snd_pcm_hw_params_set_rate(pcm, hw, rate, 0); }
  if (rc >= 0) { *stage = "ALSA period size"; rc = snd_pcm_hw_params_set_period_size_near(pcm, hw, &period, &direction); }
  if (rc >= 0) { *stage = "ALSA buffer size"; rc = snd_pcm_hw_params_set_buffer_size_near(pcm, hw, &buffer); }
  if (rc >= 0) { *stage = "ALSA apply hw params"; rc = snd_pcm_hw_params(pcm, hw); }
  if (rc < 0) return rc;
  if (period != kCapture48Frames ||
      buffer != buffer_periods * kCapture48Frames) {
    std::fprintf(stderr,
                 "boompi-client: ALSA negotiated period=%lu buffer=%lu\n",
                 static_cast<unsigned long>(period),
                 static_cast<unsigned long>(buffer));
    *stage = "ALSA exact period/buffer contract"; return -EINVAL;
  }
  snd_pcm_sw_params_t* sw = nullptr; snd_pcm_sw_params_alloca(&sw);
  *stage = "ALSA current software params"; rc = snd_pcm_sw_params_current(pcm, sw);
  if (rc >= 0) rc = snd_pcm_sw_params_set_avail_min(pcm, sw, kCapture48Frames);
  const snd_pcm_uframes_t start = stream == SND_PCM_STREAM_CAPTURE ? 1U : 3U * kCapture48Frames;
  if (rc >= 0) rc = snd_pcm_sw_params_set_start_threshold(pcm, sw, start);
  if (rc >= 0) rc = snd_pcm_sw_params(pcm, sw);
  if (rc >= 0) { *stage = "ALSA PCM prepare"; rc = snd_pcm_prepare(pcm); }
  return rc;
}

void SetLoopbackId(snd_ctl_elem_id_t* id) noexcept {
  snd_ctl_elem_id_set_interface(id, SND_CTL_ELEM_IFACE_MIXER);
  snd_ctl_elem_id_set_name(id, kLoopbackControl);
}

int OpenLoopbackControl(int card, snd_ctl_t** control, snd_ctl_elem_id_t* id,
                        snd_ctl_elem_info_t* info) noexcept {
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
                       unsigned target, bool* wrote = nullptr) noexcept {
  snd_ctl_elem_value_t* value = nullptr; snd_ctl_elem_value_alloca(&value);
  snd_ctl_elem_value_set_id(value, id); snd_ctl_elem_value_set_enumerated(value, 0U, target);
  int rc = snd_ctl_elem_write(control, value); unsigned actual = target;
  if (rc >= 0 && wrote != nullptr) *wrote = true;
  if (rc >= 0) rc = ReadLoopbackValue(control, id, &actual);
  return rc >= 0 && actual != target ? -EIO : rc;
}

int ConfigureLoopbackMode1(snd_pcm_t* capture, int* card,
                           unsigned* previous, bool* changed) noexcept {
  *card = -1; *previous = 0U; *changed = false;
  snd_pcm_info_t* pcm_info = nullptr; snd_pcm_info_alloca(&pcm_info);
  int rc = snd_pcm_info(capture, pcm_info); if (rc < 0) return rc;
  *card = snd_pcm_info_get_card(pcm_info);
  snd_ctl_t* control = nullptr; snd_ctl_elem_id_t* id = nullptr; snd_ctl_elem_info_t* info = nullptr;
  snd_ctl_elem_id_alloca(&id); snd_ctl_elem_info_alloca(&info);
  rc = OpenLoopbackControl(*card, &control, id, info); if (rc < 0) return rc;
  const unsigned items = snd_ctl_elem_info_get_items(info); unsigned target = items;
  for (unsigned item = 0U; item < items && rc >= 0; ++item) {
    snd_ctl_elem_info_set_item(info, item); rc = snd_ctl_elem_info(control, info);
    if (rc >= 0 && std::strcmp(snd_ctl_elem_info_get_item_name(info), kLoopbackMode) == 0) target = item;
  }
  if (rc >= 0 && target == items) rc = -ENOENT;
  if (rc >= 0) rc = ReadLoopbackValue(control, id, previous);
  if (rc >= 0 && *previous >= items) rc = -EINVAL;
  if (rc >= 0 && *previous != target) rc = WriteLoopbackValue(control, id, target, changed);
  snd_ctl_close(control); return rc;
}

int RestoreLoopbackMode(int card, unsigned previous) noexcept {
  snd_ctl_t* control = nullptr; snd_ctl_elem_id_t* id = nullptr; snd_ctl_elem_info_t* info = nullptr;
  snd_ctl_elem_id_alloca(&id); snd_ctl_elem_info_alloca(&info);
  int rc = OpenLoopbackControl(card, &control, id, info); if (rc < 0) return rc;
  if (previous >= snd_ctl_elem_info_get_items(info)) rc = -EINVAL;
  if (rc >= 0) rc = WriteLoopbackValue(control, id, previous);
  snd_ctl_close(control); return rc;
}

SwrContext* NewResampler(int in_rate, int in_channels, int out_rate,
                         int out_channels) noexcept {
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

}

struct AudioBackend::Impl final {
  // capture/DSP/Snowboy/VAD 归 actor；playback PCM 与重采样器归唯一播放线程。
  AudioEngineConfig config{}; snd_pcm_t *capture_pcm{nullptr}, *playback_pcm{nullptr};
  SwrContext *capture_swr{nullptr}, *playback_swr{nullptr}; RockchipVoiceDsp dsp{};
  BoompiSnowboyLegacyHandle* snowboy{nullptr}; VadInst* vad{nullptr};
  mutable std::mutex error_mutex{}; std::array<char, 192U> error{};
  std::array<std::int16_t, kCapture48Frames * kCaptureChannels> capture48{};
  std::array<std::int16_t, kVoiceFrameSamples16k * kCaptureChannels> capture16{};
  std::array<std::int16_t, kTts24Frames> playback24{};
  std::array<std::int16_t, kMaximumPlayback48Frames> playback48{};
  std::array<std::int16_t, kMaximumPlayback48Frames * kPlaybackChannels> playback_stereo{};
  VoiceFrame16k mic_left{}, mic_right{}, reference_left{}, reference_right{}, processed{};
  std::uint32_t vad_speech_ms{0U}, vad_silence_ms{0U}; int loopback_card{-1};
  unsigned loopback_previous{0U}, aec_warmup_remaining{0U}, aec_tail_remaining{0U};
  bool open{false}, vad_in_speech{false}, loopback_changed{false};
  bool playback_session_active{false}, aec_warmup_armed{false};
  std::atomic<bool> playback_interrupted{false};
  std::atomic<bool> playback_ended{false};

  void SetError(const char* text, int code = 0) noexcept {
    std::lock_guard<std::mutex> lock(error_mutex);
    if (code == 0) std::snprintf(error.data(), error.size(), "%s", text);
    else std::snprintf(error.data(), error.size(), "%s: %s", text, snd_strerror(code));
  }
  bool ResetSwr(SwrContext* swr, const std::int16_t* input, int input_frames,
                int output_frames, std::int16_t* output) noexcept {
    swr_close(swr); if (swr_init(swr) < 0) return false;
    const std::uint8_t* in[] = {reinterpret_cast<const std::uint8_t*>(input)};
    std::uint8_t* out[] = {reinterpret_cast<std::uint8_t*>(output)};
    return swr_convert(swr, out, output_frames, in, input_frames) >= 0;
  }
  bool ResetVad() noexcept {
    if (vad == nullptr || WebRtcVad_Init(vad) != 0 || WebRtcVad_set_mode(vad, config.vad_mode) != 0) return false;
    vad_speech_ms = vad_silence_ms = 0U; vad_in_speech = false; return true;
  }
  bool ResetListener() noexcept {
    int reset = 0; return snowboy != nullptr && boompi_snowboy_legacy_reset(snowboy, &reset) ==
        BOOMPI_SNOWBOY_LEGACY_OK && ResetVad();
  }
  bool ResetFrontEnd() noexcept {
    // discontinuity 后联合复位四通道重采样、3A FIFO、唤醒和 VAD 历史。
    capture48.fill(0);
    if (!ResetSwr(capture_swr, capture48.data(), kCapture48Frames,
                  kVoiceFrameSamples16k, capture16.data())) return false;
    dsp.Close(); return dsp.Open() == RockchipVoiceDspStatus::kOk && ResetListener();
  }
  bool WritePlayback(const std::int16_t* mono, std::size_t frames) noexcept {
    // TTS 是 mono；复制到 L/R，Mode1 同步回采两个数字参考。
    for (std::size_t i = 0U; i < frames; ++i) {
      playback_stereo[2U * i] = mono[i]; playback_stereo[2U * i + 1U] = mono[i];
    }
    std::size_t offset = 0U;
    while (offset < frames) {
      if (playback_interrupted.load(std::memory_order_acquire)) return false;
      const snd_pcm_sframes_t rc = snd_pcm_writei(playback_pcm,
          playback_stereo.data() + 2U * offset, frames - offset);
      if (rc > 0) { offset += static_cast<std::size_t>(rc); continue; }
      if (playback_interrupted.load(std::memory_order_acquire)) return false;
      if (rc == -EINTR) continue;
      if (rc == -EPIPE || rc == -ESTRPIPE) {
        const int recovered = snd_pcm_recover(playback_pcm, static_cast<int>(rc), 1);
        if (recovered < 0) { SetError("ALSA playback recovery", recovered); return false; }
        offset = 0U; continue;
      }
      SetError("ALSA playback write", static_cast<int>(rc)); return false;
    }
    return true;
  }
  bool UpdateVad(CaptureFrame* frame) noexcept {
    const int speech = WebRtcVad_Process(vad, 16000, processed.data(), processed.size());
    if (speech < 0) return false;
    frame->vad_now = speech == 1;
    if (frame->vad_now) { vad_speech_ms = std::min<std::uint32_t>(vad_speech_ms + kFrameMs, config.vad_start_ms); vad_silence_ms = 0U; }
    else { vad_silence_ms = std::min<std::uint32_t>(vad_silence_ms + kFrameMs, config.vad_end_ms); vad_speech_ms = 0U; }
    if (!vad_in_speech && vad_speech_ms >= config.vad_start_ms) { vad_in_speech = true; frame->vad_started = true; }
    else if (vad_in_speech && vad_silence_ms >= config.vad_end_ms) { vad_in_speech = false; frame->vad_ended = true; }
    return true;
  }

  bool UpdateNearVoice(CaptureFrame* frame) noexcept {
    // PreparePlayback 只武装预热；必须等 Mode1 真正出现参考，不能把 180 ms 首播缓存算进去。
    bool suppress = false;
    if (playback_ended.exchange(false, std::memory_order_acq_rel)) {
      playback_session_active = false;
      aec_warmup_armed = false;
      aec_warmup_remaining = 0U;
      // 打断发生前已经确认了 120 ms 近讲，必须保留其 VAD 生命周期直到 vad_ended。
      // 自然结束则隔离 300 ms 房间/扬声器尾音，结束时再清空这段抑制期的 VAD 历史。
      if (playback_interrupted.load(std::memory_order_acquire)) aec_tail_remaining = 0U;
      else { aec_tail_remaining = kAecTailFrames; suppress = true; }
    } else if (aec_warmup_armed) {
      suppress = true;
      if (ReferenceActive(reference_left) || ReferenceActive(reference_right)) {
        aec_warmup_armed = false;
        aec_warmup_remaining = kAecWarmupFrames;
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
  if (impl_->loopback_changed) {
    impl_->SetError("previous loopback mode has not been restored"); return false;
  }
  if (impl_->open) { impl_->SetError("audio backend is already open"); return false; }
  if (config.capture_pcm.empty() || config.playback_pcm.empty() || config.snowboy_resource.empty() ||
      config.snowboy_model.empty() || (config.left_polarity != 1 && config.left_polarity != -1) ||
      (config.right_polarity != 1 && config.right_polarity != -1) || config.vad_mode < 0 || config.vad_mode > 3 ||
      config.vad_start_ms == 0U || config.vad_end_ms == 0U) { impl_->SetError("invalid audio backend configuration"); return false; }
  try { impl_->config = config; } catch (...) { impl_->SetError("audio configuration allocation failed"); return false; }

  const char* stage = "ALSA capture open";
  int rc = OpenPcmHandle(config.capture_pcm, SND_PCM_STREAM_CAPTURE, &impl_->capture_pcm);
  // 驱动在 PCM open 时锁定通道布局：先用探测句柄取得 card，切 Mode1 后必须重新 open。
  if (rc >= 0) { stage = "ALSA Mode1 hardware reference"; rc = ConfigureLoopbackMode1(
      impl_->capture_pcm, &impl_->loopback_card, &impl_->loopback_previous, &impl_->loopback_changed); }
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
  impl_->playback_swr = NewResampler(24000, 1, 48000, 1); BoompiSnowboyLegacyInfo info{};
  if (impl_->capture_swr == nullptr || impl_->playback_swr == nullptr ||
      boompi_snowboy_legacy_create(config.snowboy_resource.c_str(), config.snowboy_model.c_str(),
          config.snowboy_sensitivity.c_str(), config.snowboy_gain, 0, &impl_->snowboy, &info) != BOOMPI_SNOWBOY_LEGACY_OK ||
      info.sample_rate_hz != 16000U || info.channels != 1U || info.bits_per_sample != 16U || info.keyword_count == 0U) {
    Close(); impl_->SetError("audio DSP, resampler, or Snowboy initialization failed"); return false;
  }
  impl_->vad = WebRtcVad_Create();
  if (impl_->vad == nullptr || WebRtcVad_Init(impl_->vad) != 0 || WebRtcVad_set_mode(impl_->vad, config.vad_mode) != 0 ||
      WebRtcVad_ValidRateAndFrameLength(16000, kVoiceFrameSamples16k) != 0 || !impl_->ResetFrontEnd()) {
    Close(); impl_->SetError("WebRTC VAD or voice front end initialization failed"); return false;
  }
  impl_->open = true; return true;
}

bool AudioBackend::ReadCapture20ms(bool* const discontinuity) noexcept {
  if (impl_ == nullptr || !impl_->open || discontinuity == nullptr) return false;
  *discontinuity = false; std::size_t offset = 0U;
  while (offset < kCapture48Frames) {
    const snd_pcm_sframes_t rc = snd_pcm_readi(impl_->capture_pcm,
        impl_->capture48.data() + kCaptureChannels * offset, kCapture48Frames - offset);
    if (rc > 0) { offset += static_cast<std::size_t>(rc); continue; }
    if (rc == -EINTR) continue;
    if (rc == -EPIPE || rc == -ESTRPIPE) {
      const int recovered = snd_pcm_recover(impl_->capture_pcm, static_cast<int>(rc), 1);
      if (recovered < 0) { impl_->SetError("ALSA capture recovery", recovered); return false; }
      offset = 0U; *discontinuity = true; continue;
    }
    impl_->SetError("ALSA capture read", static_cast<int>(rc)); return false;
  }
  return true;
}

bool AudioBackend::ProcessCapture20ms(bool discontinuity, CaptureFrame* const frame) noexcept {
  if (impl_ == nullptr || !impl_->open || frame == nullptr) return false;
  *frame = {};
  if (discontinuity) {
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
  if (converted != static_cast<int>(kVoiceFrameSamples16k)) { impl_->SetError("capture resampler lost frame alignment"); return false; }
  for (std::size_t i = 0U; i < kVoiceFrameSamples16k; ++i) {
    const std::size_t base = kCaptureChannels * i;
    impl_->mic_left[i] = impl_->capture16[base]; impl_->mic_right[i] = impl_->capture16[base + 1U];
    impl_->reference_left[i] = impl_->capture16[base + 2U]; impl_->reference_right[i] = impl_->capture16[base + 3U];
  }
  frame->reference_active = ReferenceActive(impl_->reference_left) ||
      ReferenceActive(impl_->reference_right);
#ifdef BOOMPI_AEC_LOOP_DIAGNOSTICS
  frame->aec_input[0] = impl_->mic_left;
  frame->aec_input[1] = impl_->mic_right;
  frame->aec_input[2] = impl_->reference_left;
  frame->aec_input[3] = impl_->reference_right;
#endif
  // 每个 sample 只进入这一条 Rockchip 3A 路径；STDT 在 vendor 内部保护双讲。
  if (impl_->dsp.Process(impl_->mic_left, impl_->mic_right, impl_->reference_left,
                         impl_->reference_right, &impl_->processed) != RockchipVoiceDspStatus::kOk) {
    impl_->SetError("Rockchip 3A rejected a frame"); return false;
  }
  std::int32_t detection = 0;
  if (boompi_snowboy_legacy_process_s16(impl_->snowboy, impl_->processed.data(), impl_->processed.size(), &detection) !=
      BOOMPI_SNOWBOY_LEGACY_OK) { impl_->SetError("Snowboy processing failed"); return false; }
  frame->wake = detection > 0;
  if (!impl_->UpdateVad(frame)) { impl_->SetError("WebRTC VAD processing failed"); return false; }
  // librkaudio 不公开 DTD 结果；近讲事件只能来自 AEC/STDT 后的真实输出。
  if (!impl_->UpdateNearVoice(frame)) { impl_->SetError("playback VAD reset failed"); return false; }
  frame->pcm = impl_->processed; return true;
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
  impl_->aec_tail_remaining = 0U;
  impl_->playback_ended.store(false, std::memory_order_release);
  impl_->playback_interrupted.store(false, std::memory_order_release);
  return true;
}

bool AudioBackend::Render20ms(const std::int16_t* const pcm24, std::size_t samples, float gain) noexcept {
  if (impl_ == nullptr || !impl_->open || pcm24 == nullptr || samples > kTts24Frames || gain < 0.0F) return false;
  impl_->playback24.fill(0); std::copy_n(pcm24, samples, impl_->playback24.begin());
  const std::uint8_t* in[] = {reinterpret_cast<const std::uint8_t*>(impl_->playback24.data())};
  std::uint8_t* out[] = {reinterpret_cast<std::uint8_t*>(impl_->playback48.data())};
  const int converted = swr_convert(impl_->playback_swr, out, impl_->playback48.size(), in, kTts24Frames);
  if (converted <= 0 || converted > static_cast<int>(impl_->playback48.size())) { impl_->SetError("TTS resampling failed"); return false; }
  long peak = 0;
  for (int i = 0; i < converted; ++i) peak = std::max(peak, std::abs(static_cast<long>(impl_->playback48[i])));
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
  const int drained = snd_pcm_drain(impl_->playback_pcm);
  if (drained < 0 && !impl_->playback_interrupted.load(std::memory_order_acquire)) impl_->SetError("ALSA playback drain", drained);
  const int prepared = snd_pcm_prepare(impl_->playback_pcm);
  if (prepared < 0 && !impl_->playback_interrupted.load(std::memory_order_acquire)) impl_->SetError("ALSA playback prepare", prepared);
  impl_->playback_ended.store(true, std::memory_order_release);
  return drained >= 0 && prepared >= 0;
}
void AudioBackend::DropPlayback() noexcept {
  if (impl_ == nullptr || impl_->playback_pcm == nullptr) return;
  const int dropped = snd_pcm_drop(impl_->playback_pcm);
  const int prepared = snd_pcm_prepare(impl_->playback_pcm);
  if (dropped < 0 && dropped != -EBADFD) impl_->SetError("ALSA playback drop", dropped);
  if (prepared < 0) impl_->SetError("ALSA playback prepare", prepared);
  impl_->playback_ended.store(true, std::memory_order_release);
}
void AudioBackend::InterruptPlayback() noexcept {
  if (impl_ == nullptr || impl_->playback_pcm == nullptr) return;
  impl_->playback_interrupted.store(true, std::memory_order_release);
  const int dropped = snd_pcm_drop(impl_->playback_pcm);
  if (dropped < 0 && dropped != -EBADFD) impl_->SetError("ALSA playback interrupt", dropped);
  impl_->playback_ended.store(true, std::memory_order_release);
}
std::string AudioBackend::last_error() const {
  if (impl_ == nullptr) return "audio backend is not allocated";
  std::lock_guard<std::mutex> lock(impl_->error_mutex); return impl_->error.data();
}
void AudioBackend::Close() noexcept {
  if (impl_ == nullptr) return;
  if (impl_->capture_pcm != nullptr) snd_pcm_close(impl_->capture_pcm);
  if (impl_->playback_pcm != nullptr) snd_pcm_close(impl_->playback_pcm);
  impl_->capture_pcm = impl_->playback_pcm = nullptr;
  if (impl_->loopback_changed) {
    int rc = RestoreLoopbackMode(impl_->loopback_card, impl_->loopback_previous);
    if (rc < 0) rc = RestoreLoopbackMode(impl_->loopback_card, impl_->loopback_previous);
    if (rc < 0) impl_->SetError("ALSA loopback restore", rc);
    else {
      impl_->loopback_card = -1;
      impl_->loopback_previous = 0U;
      impl_->loopback_changed = false;
    }
  } else {
    impl_->loopback_card = -1;
    impl_->loopback_previous = 0U;
  }
  swr_free(&impl_->capture_swr); swr_free(&impl_->playback_swr); impl_->dsp.Close();
  if (impl_->snowboy != nullptr) boompi_snowboy_legacy_destroy(impl_->snowboy);
  if (impl_->vad != nullptr) WebRtcVad_Free(impl_->vad);
  impl_->snowboy = nullptr; impl_->vad = nullptr; impl_->open = false;
  impl_->playback_session_active = impl_->aec_warmup_armed = false;
  impl_->aec_warmup_remaining = impl_->aec_tail_remaining = 0U;
  impl_->playback_ended.store(false, std::memory_order_release);
  impl_->capture48.fill(0); impl_->capture16.fill(0); impl_->mic_left.fill(0); impl_->mic_right.fill(0);
  impl_->reference_left.fill(0); impl_->reference_right.fill(0); impl_->processed.fill(0);
}

}  // namespace boompi::platform::rv1106
