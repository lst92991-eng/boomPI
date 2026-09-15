/** @file audio_thread.cpp
 * @brief RV1106 Linux 音频线程命名和实时优先级。
 *
 * 由音频工作线程自己设置 pthread_self，避免对尚未启动或已退出的线程句柄操作。
 * 名称设置结果当前忽略；调度失败会打印 warning 并继续，不改变音频功能的返回路径。
 */
#include "../../audio/audio_thread.h"

#include <pthread.h>
#include <sched.h>

#include <cstdio>

namespace boompi::audio {

void SetAudioThreadPriority(const char* name, int priority) noexcept {
  // SCHED_FIFO 优先级只决定就绪线程间的抢占；采集/播放仍需依靠 ALSA 或条件变量阻塞让出 CPU。
  static_cast<void>(pthread_setname_np(pthread_self(), name));
  sched_param parameters{};
  parameters.sched_priority = priority;
  const int result = pthread_setschedparam(pthread_self(), SCHED_FIFO, &parameters);
  if (result != 0) {
    std::fprintf(stderr, "boompi-client: warning: %s realtime priority %d failed (%d)\n", name,
                 priority, result);
  }
}

}  // namespace boompi::audio
