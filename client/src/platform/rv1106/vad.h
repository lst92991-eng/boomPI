#pragma once
#include "boompi/audio/audio_frames.h"
namespace boompi::vad {
bool open() noexcept;
bool reset() noexcept;
// -1为错误，0为无人声，1为人声；不会把错误折算为静音。
int process(const audio::VoiceFrame16k& pcm) noexcept;
void close() noexcept;
}  // namespace boompi::vad
