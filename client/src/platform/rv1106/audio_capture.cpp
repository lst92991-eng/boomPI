/** @file audio_capture.cpp
 * @brief ALSA 原始 PCM 采集：设置回采/增益/高通 → 配置输入 → 连续读取 → 中断/关闭。
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

/** @brief 配置并回读本板的回采、增益和高通；临时 mixer 句柄离开函数即释放。
 * 回读确认驱动接受该模式，实际回采链路的增益和延迟由板级配置与声学测量确定。
 */
static int configure_capture_controls()
{
    snd_ctl_t *raw;
    int rc = snd_ctl_open(&raw, board_voice::kMixerCard, 0);
    if (rc < 0)
    {
        return rc;
    }
    std::unique_ptr<snd_ctl_t, decltype(&snd_ctl_close)> control(raw, snd_ctl_close);
    snd_ctl_elem_info_t *info;
    snd_ctl_elem_value_t *value;
    snd_ctl_elem_info_alloca(&info);
    snd_ctl_elem_value_alloca(&value);
    // 枚举按名称选择，整数使用已验收的板级档位；四个控制项共用写入和回读流程。
    const struct
    {
        const char *name;
        const char *choice;
        long number;
    } settings[] = {
        {kLoopbackControl, kLoopbackMode, 0},
        {"ADC ALC Left Volume", nullptr, board_voice::kCaptureAlcVolume},
        {"ADC ALC Right Volume", nullptr, board_voice::kCaptureAlcVolume},
        {"ADC HPF Cut-off", board_voice::kCaptureHighPass, 0}
    };
    for (const auto &setting : settings)
    {
        // 每个控制项重新按名称查询，避免沿用前一次查询返回的numid。
        snd_ctl_elem_info_clear(info);
        snd_ctl_elem_info_set_interface(info, SND_CTL_ELEM_IFACE_MIXER);
        snd_ctl_elem_info_set_name(info, setting.name);
        rc = snd_ctl_elem_info(raw, info);
        if (rc < 0)
        {
            return rc;
        }
        const auto type = setting.choice ? SND_CTL_ELEM_TYPE_ENUMERATED
                                         : SND_CTL_ELEM_TYPE_INTEGER;
        if (snd_ctl_elem_info_get_type(info) != type || snd_ctl_elem_info_get_count(info) != 1)
        {
            return -EINVAL;
        }
        long target = setting.number;
        if (setting.choice)
        {
            target = -1;
            const unsigned items = snd_ctl_elem_info_get_items(info);
            for (unsigned i = 0; i < items; ++i)
            {
                snd_ctl_elem_info_set_item(info, i);
                rc = snd_ctl_elem_info(raw, info);
                if (rc < 0)
                {
                    return rc;
                }
                if (std::strcmp(snd_ctl_elem_info_get_item_name(info), setting.choice) == 0)
                {
                    target = i;
                    break;
                }
            }
            if (target < 0)
            {
                return -ENOENT;
            }
        }
        snd_ctl_elem_value_clear(value);
        snd_ctl_elem_value_set_numid(value, snd_ctl_elem_info_get_numid(info));
        if (setting.choice)
        {
            snd_ctl_elem_value_set_enumerated(value, 0, target);
        }
        else
        {
            snd_ctl_elem_value_set_integer(value, 0, target);
        }
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
        const long actual = setting.choice ? snd_ctl_elem_value_get_enumerated(value, 0)
                                            : snd_ctl_elem_value_get_integer(value, 0);
        if (actual != target)
        {
            return -EIO;
        }
    }
    return 0;
}

int open()
{
    // Mode1先于首次PCM打开；采集只有一种固定板级格式。
    int result = configure_capture_controls();
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
