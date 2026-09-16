/** @file debug.cpp
 * @brief 集中定义终端日志文案，并在 main 前注册只读回调表。
 * 调用点负责选择事件；回调只输出，不发起取消、重连或其他业务动作。
 */
#include "boompi/debug.h"

#include <cstdio>

namespace boompi::debug {
namespace {
/** @brief 返回完整回调表；无捕获 lambda 可直接转为函数指针，无额外对象或消息队列。 */
Callbacks register_callbacks() {
  Callbacks callbacks{};
  callbacks.failure = [](const char* reason) {
    std::fprintf(stderr, "boompi-client: %s\n", reason);
  };
  callbacks.offline = [](const char* stage) {
    std::fprintf(stderr, "boompi: offline; stage=%s\n", stage);
  };
  callbacks.reply_failed = [](const char* code) {
    std::fprintf(stderr, "boompi: reply failed; code=%s\n", code);
  };
  callbacks.display_unavailable = [] {
    std::fprintf(stderr, "boompi: display unavailable; voice continues\n");
  };
  callbacks.input_discontinuity = [] {
    std::fprintf(stderr, "boompi: input discontinuity; current input canceled\n");
  };
  callbacks.barge_probe = [] {
    std::fprintf(stderr, "boompi: barge probe\n");
  };
  callbacks.barge_confirmed = [] {
    std::fprintf(stderr, "boompi: barge confirmed\n");
  };
  callbacks.barge_rejected = [](bool reference_active) {
    std::fprintf(stderr, "boompi: barge rejected; reference=%d\n", reference_active);
  };
  callbacks.playback_xrun = [](std::size_t written_frames, int code) {
    std::fprintf(stderr, "boompi: playback xrun after %zu frames (%d)\n", written_frames, code);
  };
  callbacks.volume_save_failed = [] {
    std::fprintf(stderr, "boompi-ui: volume save failed\n");
  };
  callbacks.priority_failed = [](const char* thread, int priority, int code) {
    std::fprintf(stderr, "boompi-client: warning: %s realtime priority %d failed (%d)\n",
                 thread, priority, code);
  };
  callbacks.display_ready = [](unsigned spi_hz) {
    std::fprintf(stderr, "boompi-ui: SPI=%u Hz; touch=GT911/i2c-3\n", spi_hz);
  };
  callbacks.touch_disabled = [] {
    std::fprintf(stderr, "boompi-ui: GT911 disabled after bounded recovery\n");
  };
  callbacks.touch_recovery = [](const char* stage, unsigned attempt, unsigned limit) {
    std::fprintf(stderr, "boompi-ui: GT911 %s failed; recovery %u/%u\n", stage, attempt, limit);
  };
  callbacks.touch_recovered = [] {
    std::fprintf(stderr, "boompi-ui: GT911 recovered\n");
  };
  return callbacks;
}
}  // namespace
// 每次回调同步写一条终端消息，不新增业务锁或保存参数；跨线程输出由stdio串行化。
const Callbacks log = register_callbacks();
}  // namespace boompi::debug
