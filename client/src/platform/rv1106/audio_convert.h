#pragma once
#include "boompi/audio/audio_frames.h"

namespace boompi::audio_convert {
// 每个方向由所属任务独占：open成功后处理，任务退出后close；不接受未初始化调用。
bool open_capture() noexcept;
bool open_playback() noexcept;
bool reset_capture() noexcept;
bool reset_playback() noexcept;
// 保留原始四槽数据，联合降采样后直接交付交错的双麦/单参考。
bool capture(const audio::RawCaptureFrame& raw, audio::CaptureChannels& output) noexcept;
// nullptr/0取出剩余有效尾音；内部静音推进滤波器，但不把补齐静音交给声卡。
bool playback(const std::int16_t* pcm, std::size_t samples,
              audio::StereoPlaybackFrame& output) noexcept;
void close_capture() noexcept;
void close_playback() noexcept;
}  // namespace boompi::audio_convert
