#include "../../src/audio/audio_thread.h"

namespace boompi::audio {

// Host 回归使用普通线程，不要求测试机具备实时调度权限。
void SetAudioThreadPriority(const char*, int) noexcept {}

}  // namespace boompi::audio
