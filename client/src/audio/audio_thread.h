/** @file audio_thread.h
 * @brief 音频工作线程的板端调度设置。
 *
 * voice_input和playback在各自线程入口调用一次，调度设置只作用于当前线程。
 */
#pragma once

namespace audio
{

/// 设置当前线程名称及 SCHED_FIFO 优先级；无权限时记录警告，保留原有调度策略。
/// name 用于 Linux 线程诊断，priority 使用 capture=40、playback=30；不用于 UI/网络线程。
void SetAudioThreadPriority(const char *name, int priority);

}  // namespace audio
