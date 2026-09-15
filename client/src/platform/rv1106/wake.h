#pragma once
#include "boompi/audio/audio_frames.h"
namespace boompi::wake {
bool open() noexcept;
// open成功后由输入线程调用：-1错误、0未命中、1唤醒。
int detect(const audio::VoiceFrame16k& pcm) noexcept;
bool reset() noexcept;
void close() noexcept;
}  // namespace boompi::wake
