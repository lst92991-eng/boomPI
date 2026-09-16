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

namespace audio_capture
{
static snd_pcm_t *capture_pcm{nullptr};
static std::atomic<bool> capture_stopped{false};
static const char *const kLoopbackControl = "I2STDM Digital Loopback Mode";
static const char *const kLoopbackMode = "Mode1";
static const int kOpenFlags =
    SND_PCM_NO_AUTO_RESAMPLE | SND_PCM_NO_AUTO_CHANNELS | SND_PCM_NO_AUTO_FORMAT;

/** @brief 按枚举名字启用并回读 Mode1；临时 mixer 句柄离开函数即释放。
 * 回读确认驱动接受该模式，实际回采链路的增益和延迟由板级配置与声学测量确定。
 */
static int configure_loopback_mode1()
{
    snd_ctl_t *raw;
    int rc = snd_ctl_open(&raw, board_voice::kMixerCard, 0);
    if (rc < 0)
    {
        return rc;
    }
    std::unique_ptr<snd_ctl_t, decltype(&snd_ctl_close)> control(raw, snd_ctl_close);
    // control绑定snd_ctl_close，函数从任一返回点退出时都会关闭临时mixer句柄。
    snd_ctl_elem_info_t *info;
    snd_ctl_elem_value_t *value;
    snd_ctl_elem_info_alloca(&info);
    snd_ctl_elem_value_alloca(&value);
    // info读取控制项的类型和枚举说明，value保存准备写入及回读确认的枚举值。
    snd_ctl_elem_info_set_interface(info, SND_CTL_ELEM_IFACE_MIXER);
    snd_ctl_elem_info_set_name(info, kLoopbackControl);
    snd_ctl_elem_value_set_interface(value, SND_CTL_ELEM_IFACE_MIXER);
    snd_ctl_elem_value_set_name(value, kLoopbackControl);
    rc = snd_ctl_elem_info(raw, info);
    if (rc < 0)
    {
        return rc;
    }
    if (snd_ctl_elem_info_get_type(info) != SND_CTL_ELEM_TYPE_ENUMERATED ||
        snd_ctl_elem_info_get_count(info) != 1)
    {
        return -EINVAL;
    }
    // 通过Mode1枚举名称选择回采模式，使设置与驱动公开的控制项保持对应。
    const unsigned items = snd_ctl_elem_info_get_items(info);
    unsigned target = items;
    for (unsigned i = 0; i < items; ++i)
    {
        snd_ctl_elem_info_set_item(info, i);
        rc = snd_ctl_elem_info(raw, info);
        if (rc < 0)
        {
            return rc;
        }
        if (std::strcmp(snd_ctl_elem_info_get_item_name(info), kLoopbackMode) == 0)
        {
            target = i;
            break;
        }
    }
    if (target == items)
    {
        return -ENOENT;
    }
    snd_ctl_elem_value_set_enumerated(value, 0, target);
    // 写入找到的Mode1序号后立即回读，确认后续PCM使用指定的数字回采路径。
    rc = snd_ctl_elem_write(raw, value);
    if (rc < 0)
    {
        return rc;
    }
    rc = snd_ctl_elem_read(raw, value);
    if (rc < 0)
    {
        return rc;
    }
    if (snd_ctl_elem_value_get_enumerated(value, 0) != target)
    {
        return -EIO;
    }
    return rc;
}

int open()
{
    // Mode1先于首次PCM打开；采集只有一种固定板级格式。
    int result = configure_loopback_mode1();
    if (result >= 0)
    {
        result = snd_pcm_open(&capture_pcm, board_voice::kCapturePcm, SND_PCM_STREAM_CAPTURE,
                              kOpenFlags);
    }
    if (result >= 0)
    {
        result = snd_pcm_set_params(capture_pcm, SND_PCM_FORMAT_S16_LE,
                                    SND_PCM_ACCESS_RW_INTERLEAVED, audio::kCaptureChannels,
                                    audio::kDeviceRateHz, 0, 40000);
    }
    if (result < 0)
    {
        close();
    }
    capture_stopped.store(false);
    return result;
}

int read(std::int16_t *output)
{
    if (capture_stopped.load())
    {
        return -ECANCELED;
    }
    // PREPARED表示PCM已经配置，显式start后驱动开始采集，readi随后取得连续音频。
    if (snd_pcm_state(capture_pcm) == SND_PCM_STATE_PREPARED)
    {
        const int rc = snd_pcm_start(capture_pcm);
        if (rc < 0)
        {
            return rc;
        }
    }
    std::size_t offset = 0;
    // offset累计当前连续帧已经读到的采样时刻；短读后从剩余位置补齐。
    while (offset < audio::kDeviceFrameSamples)
    {
        if (capture_stopped.load())
        {
            return -ECANCELED;
        }
        const int count = static_cast<int>(
            snd_pcm_readi(capture_pcm, output + audio::kCaptureChannels * offset,
                          audio::kDeviceFrameSamples - offset));
        if (count > 0)
        {
            offset += static_cast<std::size_t>(count);
        }
        else if (capture_stopped.load())
        {
            return -ECANCELED;
        }
        else if (count == -EPIPE || count == -ESTRPIPE)
        {
            // XRUN表示时间轴断开；丢弃已读前缀，prepare后返回断点，由上层重建算法历史。
            const int rc = snd_pcm_prepare(capture_pcm);
            return rc < 0 ? rc : 0;
        }
        else if (count != -EINTR)
        {
            return count < 0 ? count : -EIO;
        }
    }
    return static_cast<int>(audio::kDeviceFrameSamples);
}

int interrupt()
{
    // 先置停止标志，再打断驱动读取，使read函数能把主动停止识别为ECANCELED。
    capture_stopped.store(true);
    return capture_pcm ? snd_pcm_abort(capture_pcm) : 0;
}

void close()
{
    if (capture_pcm)
    {
        snd_pcm_close(capture_pcm);
        capture_pcm = nullptr;
    }
}
}  // namespace audio_capture
