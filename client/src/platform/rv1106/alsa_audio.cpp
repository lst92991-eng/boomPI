/**
 * @file alsa_audio.cpp
 * @brief Mode1 声卡配置及 ALSA period 读写、恢复和中断。
 *
 * 启动先查询真实 card → 找到 Mode1 mixer 枚举 → 切换并重新 open → 严格协商 PCM。
 * 采集任务负责转换/3A，播放任务已经完成双声道转换。
 * 采集恢复必须报告断点；播放恢复记录 XRUN 并续写剩余部分，不回放已接收的前缀。
 */
#include "alsa_audio.h"

#include <alsa/asoundlib.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>

#include "board_voice_profile.h"
#include "boompi/audio/audio_format.h"

namespace boompi::alsa_audio {
namespace {
constexpr std::size_t kCaptureChannels = audio::VoiceFrameContract::capture_channels;
constexpr std::size_t kPlaybackChannels = audio::VoiceFrameContract::playback_channels;
constexpr std::size_t kCapture48Frames = audio::kCaptureFrameSamples;
// 四通道双 period 已占 15,360 字节，不能靠放大缓冲绕开采集线程的实时要求。
constexpr snd_pcm_uframes_t kCaptureBufferPeriods = 2U;
constexpr snd_pcm_uframes_t kPlaybackBufferPeriods = 4U;
constexpr const char* kLoopbackControl = "I2STDM Digital Loopback Mode";
constexpr const char* kLoopbackMode = "Mode1";

/** @brief 打开硬件格式直通 PCM；返回 ALSA 状态码，句柄由 调用模块生命周期负责。 */
int OpenPcmHandle(const std::string& name, snd_pcm_stream_t stream,
                  snd_pcm_t** output) noexcept {
  // 禁止 ALSA plug 层静默改采样率、通道和格式；硬件契约不匹配时立即暴露错误。
  constexpr int flags =
      SND_PCM_NO_AUTO_RESAMPLE | SND_PCM_NO_AUTO_CHANNELS | SND_PCM_NO_AUTO_FORMAT;
  return snd_pcm_open(output, name.c_str(), stream, flags);
}

/**
 * @brief 固定硬件格式、period、buffer 与启动门限，协商偏离契约则拒绝运行。
 * @param stage 输出失败阶段的静态文本，供 Open 与 ALSA 错误码一起写诊断。
 * @return 非负为成功，负值保留 ALSA 错误语义。
 */
int ConfigurePcm(snd_pcm_t* pcm, snd_pcm_stream_t stream, unsigned channels,
                 const char** stage) noexcept {
  // capture/playback 共用 48 kHz/S16_LE/20 ms period，只有通道数与缓冲 period 数不同。
  // 按硬件格式、缓冲、软件门限三组推进，短路后保留首个 ALSA 错误。
  snd_pcm_hw_params_t* hw = nullptr;
  snd_pcm_hw_params_alloca(&hw);
  unsigned rate = audio::VoiceFrameContract::capture_rate_hz;
  snd_pcm_uframes_t period = kCapture48Frames;
  const snd_pcm_uframes_t buffer_periods =
      stream == SND_PCM_STREAM_CAPTURE ? kCaptureBufferPeriods : kPlaybackBufferPeriods;
  snd_pcm_uframes_t buffer = buffer_periods * kCapture48Frames;
  *stage = "ALSA hardware format and buffer";
  int rc = 0;
  if ((rc = snd_pcm_hw_params_any(pcm, hw)) < 0 ||
      (rc = snd_pcm_hw_params_set_access(pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED)) < 0 ||
      (rc = snd_pcm_hw_params_set_format(pcm, hw, SND_PCM_FORMAT_S16_LE)) < 0 ||
      (rc = snd_pcm_hw_params_set_channels(pcm, hw, channels)) < 0 ||
      (rc = snd_pcm_hw_params_set_rate(pcm, hw, rate, 0)) < 0 ||
      (rc = snd_pcm_hw_params_set_period_size(pcm, hw, period, 0)) < 0) {
    return rc;
  }
  if ((rc = snd_pcm_hw_params_set_buffer_size(pcm, hw, buffer)) < 0 ||
      (rc = snd_pcm_hw_params(pcm, hw)) < 0) {
    return rc;
  }
  snd_pcm_sw_params_t* sw = nullptr;
  snd_pcm_sw_params_alloca(&sw);
  *stage = "ALSA software threshold";
  // 采集收到一个 frame 即启动；播放先在声卡中积累 60 ms。
  const snd_pcm_uframes_t start = stream == SND_PCM_STREAM_CAPTURE ? 1U : 3U * kCapture48Frames;
  if ((rc = snd_pcm_sw_params_current(pcm, sw)) < 0 ||
      (rc = snd_pcm_sw_params_set_avail_min(pcm, sw, kCapture48Frames)) < 0 ||
      (rc = snd_pcm_sw_params_set_start_threshold(pcm, sw, start)) < 0 ||
      (rc = snd_pcm_sw_params(pcm, sw)) < 0) {
    return rc;
  }
  *stage = "ALSA PCM prepare";
  return snd_pcm_prepare(pcm);
}

int ConfigureLoopbackMode1(snd_pcm_t* capture) noexcept {
  snd_pcm_info_t* pcm_info;
  snd_pcm_info_alloca(&pcm_info);
  int rc = snd_pcm_info(capture, pcm_info);
  if (rc < 0) {
    return rc;
  }
  char card[24];
  std::snprintf(card, sizeof(card), "hw:%d", snd_pcm_info_get_card(pcm_info));
  snd_ctl_t* raw;
  rc = snd_ctl_open(&raw, card, 0);
  if (rc < 0) {
    return rc;
  }
  std::unique_ptr<snd_ctl_t, decltype(&snd_ctl_close)> control(raw, snd_ctl_close);
  snd_ctl_elem_info_t* info;
  snd_ctl_elem_value_t* value;
  snd_ctl_elem_id_t* id;
  snd_ctl_elem_info_alloca(&info);
  snd_ctl_elem_value_alloca(&value);
  snd_ctl_elem_id_alloca(&id);
  snd_ctl_elem_id_set_interface(id, SND_CTL_ELEM_IFACE_MIXER);
  snd_ctl_elem_id_set_name(id, kLoopbackControl);
  snd_ctl_elem_info_set_id(info, id);
  snd_ctl_elem_value_set_id(value, id);
  rc = snd_ctl_elem_info(raw, info);
  if (rc < 0) {
    return rc;
  }
  if (snd_ctl_elem_info_get_type(info) != SND_CTL_ELEM_TYPE_ENUMERATED ||
      snd_ctl_elem_info_get_count(info) != 1) {
    return -EINVAL;
  }
  // 按枚举文本找Mode1，不依赖镜像的numid或枚举下标。
  const unsigned items = snd_ctl_elem_info_get_items(info);
  unsigned target = items;
  for (unsigned i = 0; i < items; ++i) {
    snd_ctl_elem_info_set_item(info, i);
    rc = snd_ctl_elem_info(raw, info);
    if (rc < 0) {
      return rc;
    }
    if (std::strcmp(snd_ctl_elem_info_get_item_name(info), kLoopbackMode) == 0) {
      target = i;
      break;
    }
  }
  if (target == items) {
    return -ENOENT;
  }
  rc = snd_ctl_elem_read(raw, value);
  if (rc < 0 || snd_ctl_elem_value_get_enumerated(value, 0) == target) {
    return rc;
  }
  snd_ctl_elem_value_set_enumerated(value, 0, target);
  rc = snd_ctl_elem_write(raw, value);
  if (rc < 0) {
    return rc;
  }
  rc = snd_ctl_elem_read(raw, value);
  return rc >= 0 && snd_ctl_elem_value_get_enumerated(value, 0) != target ? -EIO : rc;
}

}  // namespace

namespace {
snd_pcm_t* capture_pcm{nullptr};
snd_pcm_t* playback_pcm{nullptr};
std::atomic<bool> capture_stopped{false}, playback_stopped{false};
struct Error {
  std::mutex mutex;
  std::array<char, 192U> text{};
};
Error capture_failure, playback_failure;
bool Fail(Error& error, const char* stage, int code) noexcept {
  std::lock_guard<std::mutex> lock(error.mutex);
  std::snprintf(error.text.data(), error.text.size(), "%s: %s", stage, snd_strerror(code));
  return false;
}
std::string ReadError(Error& error) {
  std::lock_guard<std::mutex> lock(error.mutex);
  return error.text.data();
}
void ClearError(Error& error) {
  std::lock_guard<std::mutex> lock(error.mutex);
  error.text.fill('\0');
}
}  // namespace

bool open_capture(const std::string& name) noexcept {
  if (capture_pcm != nullptr) {
    return Fail(capture_failure, "ALSA capture already open", -EBUSY);
  }
  const char* stage = "ALSA capture open";
  int rc = OpenPcmHandle(name, SND_PCM_STREAM_CAPTURE, &capture_pcm);
  // Mode1在PCM open时决定通道布局，探测并设置后必须重新打开。
  if (rc >= 0) {
    stage = "ALSA Mode1 hardware reference";
    rc = ConfigureLoopbackMode1(capture_pcm);
  }
  if (rc >= 0) {
    snd_pcm_close(capture_pcm);
    capture_pcm = nullptr;
    stage = "ALSA capture reopen after Mode1";
    rc = OpenPcmHandle(name, SND_PCM_STREAM_CAPTURE, &capture_pcm);
  }
  if (rc >= 0) {
    rc = ConfigurePcm(capture_pcm, SND_PCM_STREAM_CAPTURE, kCaptureChannels, &stage);
  }
  if (rc < 0) {
    close_capture();
    return Fail(capture_failure, stage, rc);
  }
  capture_stopped.store(false);
  ClearError(capture_failure);
  return true;
}

bool open_playback(const std::string& name) noexcept {
  if (playback_pcm != nullptr) {
    return Fail(playback_failure, "ALSA playback already open", -EBUSY);
  }
  const char* stage = "ALSA playback open";
  int rc = OpenPcmHandle(name, SND_PCM_STREAM_PLAYBACK, &playback_pcm);
  if (rc >= 0) {
    rc = ConfigurePcm(playback_pcm, SND_PCM_STREAM_PLAYBACK, kPlaybackChannels, &stage);
  }
  if (rc < 0) {
    close_playback();
    return Fail(playback_failure, stage, rc);
  }
  playback_stopped.store(false);
  ClearError(playback_failure);
  return true;
}

bool read(std::int16_t* const output, bool* const discontinuity) noexcept {
  if (capture_pcm == nullptr || output == nullptr || discontinuity == nullptr) {
    return false;
  }
  *discontinuity = false;
  std::size_t offset = 0U;
  while (offset < kCapture48Frames) {
    if (capture_stopped.load()) {
      return false;
    }
    // readi 可能被信号打断或只返回部分 period；只有收齐 960 frame 才交给 DSP。
    const snd_pcm_sframes_t rc = snd_pcm_readi(capture_pcm, output + kCaptureChannels * offset,
                                               kCapture48Frames - offset);
    if (rc > 0) {
      offset += static_cast<std::size_t>(rc);
      continue;
    }
    if (capture_stopped.load()) {
      return false;
    }
    if (rc == -EINTR) {
      continue;
    }
    if (rc == -EPIPE || rc == -ESTRPIPE) {
      const int recovered = snd_pcm_recover(capture_pcm, static_cast<int>(rc), 1);
      if (recovered < 0) {
        return Fail(capture_failure, "ALSA capture recovery", recovered);
      }
      std::fprintf(stderr, "boompi-client: ALSA capture discontinuity recovered; error=%s\n",
                   snd_strerror(static_cast<int>(rc)));
      offset = 0U;
      *discontinuity = true;
      continue;
    }
    return Fail(capture_failure, "ALSA capture read", static_cast<int>(rc));
  }
  return true;
}

bool write(const std::int16_t* stereo, std::size_t frames) noexcept {
  std::size_t offset = 0U;
  while (offset < frames) {
    // ALSA 允许部分写入；offset 保证每个 sample 只进入硬件时间线一次。
    if (playback_stopped.load()) {
      return false;
    }
    const snd_pcm_sframes_t rc =
        snd_pcm_writei(playback_pcm, stereo + 2U * offset, frames - offset);
    if (rc > 0) {
      offset += static_cast<std::size_t>(rc);
      continue;
    }
    if (playback_stopped.load()) {
      return false;
    }
    if (rc == -EINTR) {
      continue;
    }
    if (rc == -EPIPE || rc == -ESTRPIPE) {
      const int recovered = snd_pcm_recover(playback_pcm, static_cast<int>(rc), 1);
      if (recovered < 0) {
        return Fail(playback_failure, "ALSA playback recovery", recovered);
      }
      // snd_pcm_writei 已接受的前缀已经进入同一条媒体时间线。恢复后只续写剩余
      // 样本；从零重写会在一次 20 ms 帧内制造可听见的重复前缀。
      std::fprintf(stderr,
                   "boompi-client: ALSA playback xrun recovered; error=%s; "
                   "accepted_frames=%zu\n",
                   snd_strerror(static_cast<int>(rc)), offset);
      continue;
    }
    return Fail(playback_failure, "ALSA playback write", static_cast<int>(rc));
  }
  return true;
}

bool prepare_playback() noexcept {
  const int prepared = snd_pcm_prepare(playback_pcm);
  if (prepared < 0) {
    return Fail(playback_failure, "ALSA playback prepare", prepared);
  }
  playback_stopped.store(false);
  return true;
}

bool drain() noexcept {
  // END表示不再写入；等待尾音，下一轮仅在PreparePlayback中准备一次。
  const int drained = snd_pcm_drain(playback_pcm);
  if (drained < 0 && !playback_interrupted()) {
    Fail(playback_failure, "ALSA playback drain", drained);
  }
  return drained >= 0;
}

void drop() noexcept {
  if (playback_pcm == nullptr) {
    return;
  }
  const int dropped = snd_pcm_drop(playback_pcm);
  // 此处由播放线程收尾；下一轮只在prepare_playback准备，避免重复初始化与取消交错。
  if (dropped < 0 && dropped != -EBADFD) {
    Fail(playback_failure, "ALSA playback drop", dropped);
  }
}

void interrupt_capture() noexcept {
  if (capture_pcm == nullptr) {
    return;
  }
  // 先记录退出意图，再 abort 正在等待的 readi；Close 在读线程退出后才释放句柄。
  capture_stopped.store(true);
  const int aborted = snd_pcm_abort(capture_pcm);
  if (aborted < 0) {
    Fail(capture_failure, "ALSA capture interrupt", aborted);
  }
}

void interrupt_playback() noexcept {
  if (playback_pcm == nullptr) {
    return;
  }
  // 先发布 interrupted，再解除阻塞；调用方因此不会把用户取消误判为硬件故障。
  playback_stopped.store(true);
  const int dropped = snd_pcm_drop(playback_pcm);
  if (dropped < 0 && dropped != -EBADFD) {
    Fail(playback_failure, "ALSA playback interrupt", dropped);
  }
}

bool playback_interrupted() noexcept {
  return playback_stopped.load();
}

std::string capture_error() {
  return ReadError(capture_failure);
}
std::string playback_error() {
  return ReadError(playback_failure);
}
void clear_playback_error() noexcept {
  ClearError(playback_failure);
}

void close_capture() noexcept {
  if (capture_pcm != nullptr) {
    snd_pcm_close(capture_pcm);
    capture_pcm = nullptr;
  }
}
void close_playback() noexcept {
  if (playback_pcm != nullptr) {
    snd_pcm_close(playback_pcm);
    playback_pcm = nullptr;
  }
}

}  // namespace boompi::alsa_audio
