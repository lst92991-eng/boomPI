/** @file audio_capture.cpp
 * @brief ALSA 原始 PCM 采集：设置 Mode1 → 配置四槽输入 → 连续读取 → 中断/关闭。
 *
 * 本层只读硬件，不调用声学算法。短读可补齐，XRUN 必须丢弃前缀并报告断点。
 * 主线程 interrupt 打断读取；采集任务退出后才能 close 释放句柄。
 */
#include "audio_capture.h"

#include <alsa/asoundlib.h>

#include <atomic>
#include <cerrno>
#include <cstring>
#include <memory>

#include "board_voice_profile.h"
#include "boompi/audio/audio_format.h"

namespace boompi::audio_capture {
namespace {
snd_pcm_t* capture_pcm{nullptr};
std::atomic<bool> capture_stopped{false};
constexpr std::size_t kCaptureChannels = audio::kCaptureChannels;
constexpr std::size_t kCapture48Frames = audio::kDeviceFrameSamples;
constexpr const char* kLoopbackControl = "I2STDM Digital Loopback Mode";
constexpr const char* kLoopbackMode = "Mode1";
constexpr int kOpenFlags =
    SND_PCM_NO_AUTO_RESAMPLE | SND_PCM_NO_AUTO_CHANNELS | SND_PCM_NO_AUTO_FORMAT;

/** @brief 按枚举名字启用并回读 Mode1；临时 mixer 句柄离开函数即释放。
 * 配置只能证明驱动接受选项，不能据此推断真实回采点、增益位置及延迟。
 */
int configure_loopback_mode1() noexcept {
  snd_ctl_t* raw;
  int rc = snd_ctl_open(&raw, audio::kMixerCard, 0);
  if (rc < 0) {
    return rc;
  }
  std::unique_ptr<snd_ctl_t, decltype(&snd_ctl_close)> control(raw, snd_ctl_close);
  snd_ctl_elem_info_t* info;
  snd_ctl_elem_value_t* value;
  snd_ctl_elem_info_alloca(&info);
  snd_ctl_elem_value_alloca(&value);
  snd_ctl_elem_info_set_interface(info, SND_CTL_ELEM_IFACE_MIXER);
  snd_ctl_elem_info_set_name(info, kLoopbackControl);
  snd_ctl_elem_value_set_interface(value, SND_CTL_ELEM_IFACE_MIXER);
  snd_ctl_elem_value_set_name(value, kLoopbackControl);
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
  snd_ctl_elem_value_set_enumerated(value, 0, target);
  rc = snd_ctl_elem_write(raw, value);
  if (rc < 0) {
    return rc;
  }
  rc = snd_ctl_elem_read(raw, value);
  if (rc < 0) {
    return rc;
  }
  if (snd_ctl_elem_value_get_enumerated(value, 0) != target) {
    return -EIO;
  }
  return rc;
}

}  // namespace

int open() noexcept {
  // Mode1先于首次PCM打开；采集只有一种固定板级格式。
  int result = configure_loopback_mode1();
  if (result >= 0) {
    result = snd_pcm_open(&capture_pcm, audio::kCapturePcm, SND_PCM_STREAM_CAPTURE, kOpenFlags);
  }
  if (result >= 0) {
    result =
        snd_pcm_set_params(capture_pcm, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED,
                           audio::kCaptureChannels, audio::kDeviceRateHz, 0, 40000);
  }
  if (result < 0) {
    close();
  }
  capture_stopped.store(false);
  return result;
}

int read(std::int16_t* output) noexcept {
  if (capture_stopped.load()) {
    return -ECANCELED;
  }
  // 显式启动采集，避免把set_params的播放预缓冲门限当成采集启动条件。
  if (snd_pcm_state(capture_pcm) == SND_PCM_STATE_PREPARED) {
    const int rc = snd_pcm_start(capture_pcm);
    if (rc < 0) {
      return rc;
    }
  }
  std::size_t offset = 0;
  while (offset < kCapture48Frames) {
    if (capture_stopped.load()) {
      return -ECANCELED;
    }
    const int count = static_cast<int>(snd_pcm_readi(
        capture_pcm, output + kCaptureChannels * offset, kCapture48Frames - offset));
    if (count > 0) {
      offset += static_cast<std::size_t>(count);
    } else if (capture_stopped.load()) {
      return -ECANCELED;
    } else if (count == -EPIPE || count == -ESTRPIPE) {
      // 丢弃短读前缀，立即报告断点；不等待resume，不拼接恢复后的音频。
      const int rc = snd_pcm_prepare(capture_pcm);
      return rc < 0 ? rc : 0;
    } else if (count != -EINTR) {
      return count < 0 ? count : -EIO;
    }
  }
  return static_cast<int>(kCapture48Frames);
}

int interrupt() noexcept {
  capture_stopped.store(true);
  return capture_pcm ? snd_pcm_abort(capture_pcm) : 0;
}

void close() noexcept {
  if (capture_pcm) {
    snd_pcm_close(capture_pcm);
    capture_pcm = nullptr;
  }
}
}  // namespace boompi::audio_capture
