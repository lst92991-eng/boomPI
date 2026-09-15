#pragma once
#include "boompi/audio/audio_frames.h"
#include "playback_state.h"
namespace boompi::vad {
bool open() noexcept;
bool reset() noexcept;
// 同帧PCM执行VAD/句尾滞回，并用实际回采与播放事实产生近讲候选；不负责确认打断。
bool process(audio::CaptureFrame* frame, const playback::Observation& playback) noexcept;
void arm_playback() noexcept;
void discontinuity() noexcept;
const char* error() noexcept;
void close() noexcept;
}  // namespace boompi::vad
