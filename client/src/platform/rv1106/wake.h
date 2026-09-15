#pragma once
#include "boompi/audio/audio_frames.h"
namespace boompi::wake {
bool open() noexcept;
bool detect(const audio::VoiceFrame16k& pcm, bool* detected) noexcept;
bool reset() noexcept;
const char* error() noexcept;
void close() noexcept;
}  // namespace boompi::wake
