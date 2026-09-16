#pragma once
#include <cstddef>

namespace boompi::debug {
// debug.cpp在进入main前注册全部回调，运行期只读；业务只交付事件和必要参数。
struct Callbacks {
  void (*failure)(const char* reason);
  void (*offline)(const char* stage);
  void (*reply_failed)(const char* code);
  void (*display_unavailable)();
  void (*input_discontinuity)();
  void (*barge_probe)();
  void (*barge_confirmed)();
  void (*barge_rejected)(bool reference_active);
  void (*playback_xrun)(std::size_t written_frames, int code);
  void (*volume_save_failed)();
  void (*display_worker_failed)();
  void (*priority_failed)(const char* thread, int priority, int code);
  void (*display_ready)(unsigned spi_hz);
  void (*touch_disabled)();
  void (*touch_recovery)(const char* stage, unsigned attempt, unsigned limit);
  void (*touch_recovered)();
};
extern const Callbacks log;
}  // namespace boompi::debug
