#pragma once

namespace boompi::playback {

enum class End { None, Natural, Interrupted };
struct Observation final {
  bool render_started{false};
  bool output_audible{false};
  End end{End::None};
};

// 采集线程每帧读取，end只消费一次；这是渲染事实，真实出声仍须Mode1参考确认。
Observation observe();

}  // namespace boompi::playback
