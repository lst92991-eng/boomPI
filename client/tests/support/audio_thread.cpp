/**
 * @file audio_thread.cpp
 * @brief Host 的线程优先级替身；CMake 选入此文件，回归无需实时调度权限。
 *
 * AudioTasks 的线程创建、队列与停止顺序仍然是真实实现，仅省略 Linux FIFO 调度设置。
 */
#include "../../src/audio/audio_thread.h"

namespace boompi::audio {

// Host 回归使用普通线程，不要求测试机具备实时调度权限。
void SetAudioThreadPriority(const char*, int) noexcept {}

}  // namespace boompi::audio
