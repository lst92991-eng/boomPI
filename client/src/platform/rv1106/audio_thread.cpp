/** @file audio_thread.cpp
 * @brief RV1106 Linux 音频线程命名和实时优先级。
 */
#include "../../audio/audio_thread.h"

#include <pthread.h>
#include <sched.h>

#include <cstdio>

namespace boompi::audio {

void SetAudioThreadPriority(const char* name, int priority) noexcept {
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
