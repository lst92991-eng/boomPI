#pragma once
#include <atomic>

namespace boompi::test::audio_vendor {
// 仅控制第三方C边界；生产分块、准入计数、时间对齐和播放门控不允许测试改写。
extern std::atomic<int> vad_result, wake_result;
extern std::atomic<bool> snowboy_process_ok;
extern std::atomic<unsigned> dsp_calls, dsp_failure_call;
void reset() noexcept;
}  // namespace boompi::test::audio_vendor
