/** @file audio_frames.h
 * @brief 音频各处理阶段交接的数据；采样格式由 audio_format.h 统一定义。
 *
 * 输入线程依次处理RawCaptureFrame → CaptureChannels → CaptureFrame。
 * 输入线程依次调用3A、wake、VAD；应用只整理语句并交付网络。
 * 输出链为 16 kHz TTS → StereoPlaybackFrame → ALSA。
 */
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "boompi/audio/audio_format.h"

namespace audio
{

// ALSA四槽[mic0,mic1,refL,refR]；转换后仍交错为厂商需要的[mic0,mic1,refL]。
typedef std::array<std::int16_t, kDeviceFrameSamples * kCaptureChannels> RawCaptureFrame;
typedef std::array<std::int16_t, kVoiceFrameSamples * 3> CaptureChannels;

/// 输入线程完成一块PCM及检测；语句判断不回写这份交接数据。
struct CaptureFrame final
{
    VoiceFrame16k pcm{};
    bool wake{false}, vad_now{false};
    // 参考活动和静音写入观测按3A输出延迟对齐，供语句模块判断当前帧的播放影响。
    bool reference_active{false}, playback_held{false};
    // 硬件断流或交接溢出时置位，应用据此结束当前语句并重新等待连续输入。
    bool discontinuity{false};
};

// 每次最多交付一块48k双声道PCM；滤波尾音通过后续调用继续取出。
const std::size_t kPlaybackFrameCapacity = kDeviceFrameSamples;
/// @brief 48 kHz 交错双声道输出；frames 数的是采样时刻，实际 S16 元素数为 frames×2。
struct StereoPlaybackFrame final
{
    std::array<std::int16_t, kPlaybackFrameCapacity * kPlaybackChannels> pcm{};
    std::size_t frames{0U};
};

}  // namespace audio
