#pragma once
#include "boompi/audio/audio_frames.h"

namespace boompi::audio_convert {
float ac_rms_dbfs(const audio::VoiceFrame16k& samples) noexcept;
bool open_capture(std::int8_t left_polarity, std::int8_t right_polarity) noexcept;
bool open_playback() noexcept;
bool reset_capture() noexcept;
bool reset_playback() noexcept;
// 保留原始四槽数据，联合降采样后拆成双麦/单参考，并计算原始麦电平。
bool capture(const audio::RawCaptureFrame& raw, audio::CaptureChannels* output) noexcept;
// nullptr/0取出剩余有效尾音；内部静音推进滤波器，但不把补齐静音交给声卡。
bool playback(const std::int16_t* pcm, std::size_t samples,
              audio::StereoPlaybackFrame* output) noexcept;
long peak(const audio::StereoPlaybackFrame& frame) noexcept;
void apply_volume(audio::StereoPlaybackFrame* frame, float gain, long peak) noexcept;
void close_capture() noexcept;
void close_playback() noexcept;
}  // namespace boompi::audio_convert
