/** @file audio_thread.h
 * @brief 音频工作线程的板端调度设置。
 */
#pragma once

namespace boompi::audio {

/// 设置当前线程名称及 SCHED_FIFO 优先级；无权限时记录警告，继续使用普通调度。
void SetAudioThreadPriority(const char* name, int priority) noexcept;

}  // namespace boompi::audio
