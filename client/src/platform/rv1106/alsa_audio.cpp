/// @file Mode1 声卡配置及 ALSA period 读写；语音算法和重采样不进入本模块。
#include "alsa_audio.h"

#include <cerrno>
#include <cstdio>
#include <cstring>

#include "boompi/audio/board_voice_profile.h"

namespace boompi::platform::rv1106 {
namespace {
constexpr std::size_t kCaptureChannels =
    audio::VoiceFrameContract::capture_channels;
constexpr std::size_t kPlaybackChannels =
    audio::VoiceFrameContract::playback_channels;
constexpr std::size_t kCapture48Frames = audio::kCaptureFrameSamples;
// 四通道双 period 已占 15,360 字节，不能靠放大缓冲绕开采集线程的实时要求。
constexpr snd_pcm_uframes_t kCaptureBufferPeriods = 2U;
constexpr snd_pcm_uframes_t kPlaybackBufferPeriods = 4U;
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
  snd_pcm_hw_params_t* hw = nullptr;
  snd_pcm_hw_params_alloca(&hw);
  unsigned rate = audio::VoiceFrameContract::capture_rate_hz;
  snd_pcm_uframes_t period = kCapture48Frames;
  const snd_pcm_uframes_t buffer_periods =
      stream == SND_PCM_STREAM_CAPTURE ? kCaptureBufferPeriods
                                       : kPlaybackBufferPeriods;
  unsigned period_count = static_cast<unsigned>(buffer_periods);
  snd_pcm_uframes_t buffer = buffer_periods * kCapture48Frames;
  int direction = 0;
  *stage = "ALSA hw params any";
  int rc = snd_pcm_hw_params_any(pcm, hw);
  if (rc >= 0) {
    *stage = "ALSA interleaved access";
    rc = snd_pcm_hw_params_set_access(
        pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED);
  }
  if (rc >= 0) {
    *stage = "ALSA S16 format";
    rc = snd_pcm_hw_params_set_format(pcm, hw, SND_PCM_FORMAT_S16_LE);
  }
  if (rc >= 0) {
    *stage = "ALSA channel count";
    rc = snd_pcm_hw_params_set_channels(pcm, hw, channels);
  }
  if (rc >= 0) {
    *stage = "ALSA 48 kHz rate";
    rc = snd_pcm_hw_params_set_rate(pcm, hw, rate, 0);
  }
  if (rc >= 0) {
    *stage = "ALSA period size";
    rc = snd_pcm_hw_params_set_period_size_near(
        pcm, hw, &period, &direction);
  }
  direction = 0;
  if (rc >= 0) {
    *stage = "ALSA period count";
    rc = snd_pcm_hw_params_set_periods_near(
        pcm, hw, &period_count, &direction);
  }
  if (rc >= 0) {
    *stage = "ALSA buffer size";
    rc = snd_pcm_hw_params_set_buffer_size_near(pcm, hw, &buffer);
  }
  if (rc >= 0) {
    *stage = "ALSA apply hw params";
    rc = snd_pcm_hw_params(pcm, hw);
  }
  if (rc < 0) return rc;
  if (period != kCapture48Frames || period_count != buffer_periods ||
      buffer != buffer_periods * kCapture48Frames) {
    // *_near 允许驱动协商相邻值；产品算法依赖严格 20 ms，协商结果必须再次验证。
    std::fprintf(stderr,
                 "boompi-client: ALSA negotiated period=%lu periods=%u buffer=%lu\n",
                 static_cast<unsigned long>(period),
                 period_count,
                 static_cast<unsigned long>(buffer));
    *stage = "ALSA exact period/buffer contract";
    return -EINVAL;
  }
  snd_pcm_sw_params_t* sw = nullptr;
  snd_pcm_sw_params_alloca(&sw);
  *stage = "ALSA current software params";
  rc = snd_pcm_sw_params_current(pcm, sw);
  if (rc >= 0) rc = snd_pcm_sw_params_set_avail_min(pcm, sw, kCapture48Frames);
  // capture 有一个 frame 即启动；playback 先在内核积累 60 ms，降低首播 XRUN 概率。
  const snd_pcm_uframes_t start =
      stream == SND_PCM_STREAM_CAPTURE ? 1U : 3U * kCapture48Frames;
  if (rc >= 0) rc = snd_pcm_sw_params_set_start_threshold(pcm, sw, start);
  if (rc >= 0) rc = snd_pcm_sw_params(pcm, sw);
  if (rc >= 0) {
    *stage = "ALSA PCM prepare";
    rc = snd_pcm_prepare(pcm);
  }
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
  std::array<char, 24U> name{};
  std::snprintf(name.data(), name.size(), "hw:%d", card);
  int rc = snd_ctl_open(control, name.data(), 0);
  if (rc < 0) return rc;
  SetLoopbackId(id);
  snd_ctl_elem_info_set_id(info, id);
  rc = snd_ctl_elem_info(*control, info);
  if (rc >= 0 &&
      (snd_ctl_elem_info_get_type(info) != SND_CTL_ELEM_TYPE_ENUMERATED ||
       snd_ctl_elem_info_get_count(info) != 1U)) {
    rc = -EINVAL;
  }
  if (rc < 0) {
    snd_ctl_close(*control);
    *control = nullptr;
  }
  return rc;
}

int ReadLoopbackValue(snd_ctl_t* control, snd_ctl_elem_id_t* id,
                      unsigned* output) noexcept {
  snd_ctl_elem_value_t* value = nullptr;
  snd_ctl_elem_value_alloca(&value);
  snd_ctl_elem_value_set_id(value, id);
  const int rc = snd_ctl_elem_read(control, value);
  if (rc >= 0) *output = snd_ctl_elem_value_get_enumerated(value, 0U);
  return rc;
}

int WriteLoopbackValue(snd_ctl_t* control, snd_ctl_elem_id_t* id,
                       unsigned target) noexcept {
  snd_ctl_elem_value_t* value = nullptr;
  snd_ctl_elem_value_alloca(&value);
  snd_ctl_elem_value_set_id(value, id);
  snd_ctl_elem_value_set_enumerated(value, 0U, target);
  int rc = snd_ctl_elem_write(control, value);
  unsigned actual = target;
  if (rc >= 0) rc = ReadLoopbackValue(control, id, &actual);
  return rc >= 0 && actual != target ? -EIO : rc;
}

int ConfigureLoopbackMode1(snd_pcm_t* capture) noexcept {
  // 先从已打开 PCM 查询真实声卡，再按枚举文本选择 Mode1，兼容枚举序号变化。
  snd_pcm_info_t* pcm_info = nullptr;
  snd_pcm_info_alloca(&pcm_info);
  int rc = snd_pcm_info(capture, pcm_info);
  if (rc < 0) return rc;
  const int card = snd_pcm_info_get_card(pcm_info);
  snd_ctl_t* control = nullptr;
  snd_ctl_elem_id_t* id = nullptr;
  snd_ctl_elem_info_t* info = nullptr;
  snd_ctl_elem_id_alloca(&id);
  snd_ctl_elem_info_alloca(&info);
  rc = OpenLoopbackControl(card, &control, id, info);
  if (rc < 0) return rc;
  const unsigned items = snd_ctl_elem_info_get_items(info);
  unsigned target = items;
  for (unsigned item = 0U; item < items && rc >= 0; ++item) {
    snd_ctl_elem_info_set_item(info, item);
    rc = snd_ctl_elem_info(control, info);
    if (rc >= 0 &&
        std::strcmp(snd_ctl_elem_info_get_item_name(info), kLoopbackMode) == 0)
      target = item;
  }
  if (rc >= 0 && target == items) rc = -ENOENT;
  unsigned current = target;
  if (rc >= 0) rc = ReadLoopbackValue(control, id, &current);
  if (rc >= 0 && current != target) rc = WriteLoopbackValue(control, id, target);
  snd_ctl_close(control);
  return rc;
}

}  // namespace

void AlsaAudio::SetError(const char* stage, int code) noexcept {
  std::lock_guard<std::mutex> lock(error_mutex_);
  std::snprintf(error_.data(), error_.size(), "%s: %s", stage, snd_strerror(code));
}

bool AlsaAudio::Open(const std::string& capture_name,
                     const std::string& playback_name) noexcept {
  // 先设置 Mode1 通道布局，再配置正式 PCM；调用方在声卡就绪后才创建语音算法。
  const char* stage = "ALSA capture open";
  int rc = OpenPcmHandle(capture_name, SND_PCM_STREAM_CAPTURE, &capture_pcm_);
  // 驱动在 PCM open 时锁定通道布局：先用探测句柄取得 card，切 Mode1 后必须重新 open。
  if (rc >= 0) {
    stage = "ALSA Mode1 hardware reference";
    rc = ConfigureLoopbackMode1(capture_pcm_);
  }
  if (rc >= 0) {
    snd_pcm_close(capture_pcm_);
    capture_pcm_ = nullptr;
    stage = "ALSA capture reopen after Mode1";
    rc = OpenPcmHandle(capture_name, SND_PCM_STREAM_CAPTURE, &capture_pcm_);
  }
  if (rc >= 0) {
    stage = "ALSA capture exact 48k/4ch/960/1920 setup";
    rc = ConfigurePcm(capture_pcm_, SND_PCM_STREAM_CAPTURE, kCaptureChannels, &stage);
  }
  if (rc >= 0) {
    stage = "ALSA playback open";
    rc = OpenPcmHandle(playback_name, SND_PCM_STREAM_PLAYBACK, &playback_pcm_);
  }
  if (rc >= 0) {
    stage = "ALSA playback exact 48k/2ch/960/3840 setup";
    rc = ConfigurePcm(playback_pcm_, SND_PCM_STREAM_PLAYBACK, kPlaybackChannels, &stage);
  }
  if (rc < 0) {
    Close();
    SetError(stage, rc);
    return false;
  }

  capture_interrupted_.store(false, std::memory_order_release);
  playback_interrupted_.store(false, std::memory_order_release);
  playback_xruns_.store(0U, std::memory_order_release);
  std::lock_guard<std::mutex> lock(error_mutex_);
  error_.fill('\0');
  return true;
}

bool AlsaAudio::ReadCapture20ms(
    std::int16_t* const output, bool* const discontinuity) noexcept {
  if (capture_pcm_ == nullptr || output == nullptr || discontinuity == nullptr)
    return false;
  *discontinuity = false;
  std::size_t offset = 0U;
  while (offset < kCapture48Frames) {
    // readi 可能被信号打断或只返回部分 period；只有收齐 960 frame 才交给 DSP。
    const snd_pcm_sframes_t rc = snd_pcm_readi(capture_pcm_,
        output + kCaptureChannels * offset, kCapture48Frames - offset);
    if (rc > 0) {
      offset += static_cast<std::size_t>(rc);
      continue;
    }
    if (capture_interrupted_.load(std::memory_order_acquire)) return false;
    if (rc == -EINTR) continue;
    if (rc == -EPIPE || rc == -ESTRPIPE) {
      const int recovered = snd_pcm_recover(capture_pcm_, static_cast<int>(rc), 1);
      if (recovered < 0) {
        SetError("ALSA capture recovery", recovered);
        return false;
      }
      std::fprintf(stderr, "boompi-client: ALSA capture discontinuity recovered; error=%s\n",
                   snd_strerror(static_cast<int>(rc)));
      offset = 0U;
      *discontinuity = true;
      continue;
    }
    SetError("ALSA capture read", static_cast<int>(rc));
    return false;
  }
  return true;
}

bool AlsaAudio::WritePlayback(
    const std::int16_t* stereo, std::size_t frames) noexcept {
  std::size_t offset = 0U;
  while (offset < frames) {
    // ALSA 允许部分写入；offset 保证每个 sample 只进入硬件时间线一次。
    if (playback_interrupted_.load(std::memory_order_acquire)) return false;
    const snd_pcm_sframes_t rc = snd_pcm_writei(playback_pcm_,
        stereo + 2U * offset, frames - offset);
    if (rc > 0) {
      offset += static_cast<std::size_t>(rc);
      continue;
    }
    if (playback_interrupted_.load(std::memory_order_acquire)) return false;
    if (rc == -EINTR) continue;
    if (rc == -EPIPE || rc == -ESTRPIPE) {
      const int recovered = snd_pcm_recover(playback_pcm_, static_cast<int>(rc), 1);
      if (recovered < 0) {
        SetError("ALSA playback recovery", recovered);
        return false;
      }
      playback_xruns_.fetch_add(1U, std::memory_order_relaxed);
      // snd_pcm_writei 已接受的前缀已经进入同一条媒体时间线。恢复后只续写剩余
      // 样本；从零重写会在一次 20 ms 帧内制造可听见的重复前缀。
      std::fprintf(stderr,
                   "boompi-client: ALSA playback xrun recovered; error=%s; "
                   "accepted_frames=%zu\n",
                   snd_strerror(static_cast<int>(rc)), offset);
      continue;
    }
    SetError("ALSA playback write", static_cast<int>(rc));
    return false;
  }
  return true;
}

bool AlsaAudio::PreparePlayback() noexcept {
  const int prepared = snd_pcm_prepare(playback_pcm_);
  if (prepared < 0) {
    SetError("ALSA playback prepare", prepared);
    return false;
  }
  playback_interrupted_.store(false, std::memory_order_release);
  return true;
}

void AlsaAudio::ClearError() noexcept {
  std::lock_guard<std::mutex> lock(error_mutex_);
  error_.fill('\0');
}

bool AlsaAudio::DrainPlayback() noexcept {
  const int drained = snd_pcm_drain(playback_pcm_);
  if (drained < 0 && !playback_interrupted()) SetError("ALSA playback drain", drained);
  const int prepared = snd_pcm_prepare(playback_pcm_);
  if (prepared < 0 && !playback_interrupted()) SetError("ALSA playback prepare", prepared);
  return drained >= 0 && prepared >= 0;
}

void AlsaAudio::DropPlayback() noexcept {
  if (playback_pcm_ == nullptr) return;
  const int dropped = snd_pcm_drop(playback_pcm_);
  const int prepared = snd_pcm_prepare(playback_pcm_);
  if (dropped < 0 && dropped != -EBADFD) SetError("ALSA playback drop", dropped);
  if (prepared < 0) SetError("ALSA playback prepare", prepared);
}

void AlsaAudio::InterruptCapture() noexcept {
  if (capture_pcm_ == nullptr) return;
  capture_interrupted_.store(true, std::memory_order_release);
  const int aborted = snd_pcm_abort(capture_pcm_);
  if (aborted < 0) SetError("ALSA capture interrupt", aborted);
}

void AlsaAudio::InterruptPlayback() noexcept {
  if (playback_pcm_ == nullptr) return;
  // 先发布 interrupted，再解除阻塞；调用方因此不会把用户取消误判为硬件故障。
  playback_interrupted_.store(true, std::memory_order_release);
  const int dropped = snd_pcm_drop(playback_pcm_);
  if (dropped < 0 && dropped != -EBADFD) SetError("ALSA playback interrupt", dropped);
}

bool AlsaAudio::playback_interrupted() const noexcept {
  return playback_interrupted_.load(std::memory_order_acquire);
}

std::uint64_t AlsaAudio::playback_xruns() const noexcept {
  return playback_xruns_.load(std::memory_order_acquire);
}

std::string AlsaAudio::last_error() const {
  std::lock_guard<std::mutex> lock(error_mutex_);
  return error_.data();
}

void AlsaAudio::Close() noexcept {
  // Mode1 是板卡运行状态，退出无需切回会破坏四通道布局的默认模式。
  if (capture_pcm_ != nullptr) snd_pcm_close(capture_pcm_);
  if (playback_pcm_ != nullptr) snd_pcm_close(playback_pcm_);
  capture_pcm_ = playback_pcm_ = nullptr;
}

}  // namespace boompi::platform::rv1106
