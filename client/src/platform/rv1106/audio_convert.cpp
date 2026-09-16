/** @file audio_convert.cpp
 * @brief 当前硬件格式与算法格式之间的适配；两个方向各自保留滤波历史。
 *
 * 采集：校正双麦极性 → 双麦/refL共同48k→16k。
 * 播放：16k单声道 → 48k交错双声道；结束时单独取出滤波尾音。
 * 两个SwrContext分别保存输入和输出的滤波历史，连接48k设备格式与16k业务格式。
 */
#include "audio_convert.h"

#include "board_voice_profile.h"

extern "C"
{
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
}

#include <algorithm>

namespace audio_convert
{
// capture和playback各自只由对应线程使用，启动/退出时分别开关，不共享滤波历史。
static SwrContext *capture_swr{nullptr};
static SwrContext *playback_swr{nullptr};
static std::size_t playback_pending_frames{0U};

/** @brief 只分配 S16 交错转换器；矩阵设置及初始化由对应方向的 open/reset 完成。 */
static SwrContext *create_resampler(int in_rate, int in_channels, int out_rate,
                                    int out_channels)
{
    return swr_alloc_set_opts(
        nullptr, av_get_default_channel_layout(out_channels), AV_SAMPLE_FMT_S16, out_rate,
        av_get_default_channel_layout(in_channels), AV_SAMPLE_FMT_S16, in_rate, 0, nullptr);
}

bool open_capture()
{
    // 建立输入转换器后设置通道矩阵，再初始化滤波历史，三路有效信号保持同一时间轴。
    capture_swr = create_resampler(audio::kDeviceRateHz, 4, audio::kVoiceRateHz, 3);
    // 每行对应一个输出通道；最后一列全零，refR不进入算法。
    // clang-format off
  const double channels[] = {
      board_voice::kLeftMicPolarity, 0, 0, 0,   // mic0
      0, board_voice::kRightMicPolarity, 0, 0,  // mic1
      0, 0, 1, 0                               // refL
  };
    // clang-format on
    if (!capture_swr || swr_set_matrix(capture_swr, channels, 4) < 0 || !reset_capture())
    {
        close_capture();
        return false;
    }
    return true;
}

bool open_playback()
{
    playback_swr = create_resampler(audio::kVoiceRateHz, 1, audio::kDeviceRateHz, 2);
    // 初始化前设置L=mono、R=mono，让同一语音以相同幅度送入左右两个硬件通道。
    const double stereo_matrix[] = {1.0, 1.0};
    if (!playback_swr || swr_set_matrix(playback_swr, stereo_matrix, 1) < 0 ||
        swr_init(playback_swr) < 0)
    {
        close_playback();
        return false;
    }
    return true;
}

bool reset_capture()
{
    // swr_close保留转换配置，swr_init重新建立干净的滤波状态，供连续帧重新起步。
    swr_close(capture_swr);
    if (swr_init(capture_swr) < 0)
    {
        return false;
    }
    // 送入一块静音建立滤波历史，后续每次960点输入才能稳定交付320点。
    const audio::RawCaptureFrame silence{};
    const std::uint8_t *in[] = {reinterpret_cast<const std::uint8_t *>(silence.data())};
    audio::CaptureChannels discarded;
    std::uint8_t *out[] = {reinterpret_cast<std::uint8_t *>(discarded.data())};
    return swr_convert(capture_swr, out, audio::kVoiceFrameSamples, in,
                       audio::kDeviceFrameSamples) >= 0;
}

bool reset_playback()
{
    swr_close(playback_swr);
    playback_pending_frames = 0U;
    return swr_init(playback_swr) >= 0;
}

bool capture(const audio::RawCaptureFrame &raw, audio::CaptureChannels &output)
{
    // 直接消费原始交错PCM，库内一次完成选通道、极性与共同重采样。
    const std::uint8_t *in[] = {reinterpret_cast<const std::uint8_t *>(raw.data())};
    std::uint8_t *out[] = {reinterpret_cast<std::uint8_t *>(output.data())};
    const int converted = swr_convert(capture_swr, out, audio::kVoiceFrameSamples, in,
                                      audio::kDeviceFrameSamples);
    return converted == static_cast<int>(audio::kVoiceFrameSamples);
}

bool playback(const std::int16_t *pcm, std::size_t samples, audio::StereoPlaybackFrame &output)
{
    output.frames = 0U;
    // FFmpeg的null flush可能吞掉不足滤波半窗的极短输入。
    // 句尾用只读静音推进滤波历史，按尚欠的有效采样时刻限制输出长度。
    static const std::array<std::int16_t, audio::kVoiceFrameSamples> silence{};
    const std::size_t ratio = audio::kDeviceRateHz / audio::kVoiceRateHz;
    // 48k/16k的比例为3，真实输入决定剩余有效输出时刻，从而保持回答的原始时长。
    playback_pending_frames += samples * ratio;
    // 欠输出数量按48k采样时刻计算，记录进入滤波器但尚未交给声卡的有效音频。
    if (playback_pending_frames > 2 * audio::kPlaybackFrameCapacity)
    {
        return false;
    }
    if (playback_pending_frames == 0U)
    {
        return true;
    }
    const bool ending = pcm == nullptr;
    if (ending)
    {
        // 句尾喂入一块静音推动滤波器吐出历史采样，实际输出仍受有效欠量限制。
        pcm = silence.data();
        samples = silence.size();
    }
    const std::uint8_t *in[] = {reinterpret_cast<const std::uint8_t *>(pcm)};
    std::uint8_t *out[] = {reinterpret_cast<std::uint8_t *>(output.pcm.data())};
    const int converted = swr_convert(
        playback_swr, out,
        static_cast<int>(std::min(playback_pending_frames, audio::kPlaybackFrameCapacity)), in,
        static_cast<int>(samples));
    if (converted < 0 || (ending && converted == 0))
    {
        return false;
    }
    output.frames = static_cast<std::size_t>(converted);
    // 已交付时刻从欠量中扣除，下一次尾播调用继续处理剩余部分。
    playback_pending_frames -= output.frames;
    return true;
}

void close_capture()
{
    swr_free(&capture_swr);
}

void close_playback()
{
    swr_free(&playback_swr);
    playback_pending_frames = 0U;
}

}  // namespace audio_convert
