/** @file debug.h
 * @brief 终端日志的只读回调表；调用者传事件及参数，格式与输出位置在 debug.cpp。
 * 回调同步使用参数、不保留指针，也不改变业务状态；不能在信号处理函数中调用。
 */
#pragma once
#include <cstddef>

namespace boompi::debug {
// debug.cpp在进入main前注册全部回调，运行期只读；业务只交付事件和必要参数。
/** @brief 按进程、问答、音频、界面分类的日志入口；所有字段在 main 前一次初始化。 */
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
  void (*priority_failed)(const char* thread, int priority, int code);
  void (*display_ready)(unsigned spi_hz);
  void (*touch_disabled)();
  void (*touch_recovery)(const char* stage, unsigned attempt, unsigned limit);
  void (*touch_recovered)();
};
extern const Callbacks log;
}  // namespace boompi::debug
