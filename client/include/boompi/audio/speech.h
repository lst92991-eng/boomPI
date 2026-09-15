#pragma once
#include "boompi/audio/audio_frames.h"

namespace boompi::speech {
inline constexpr std::size_t kPreRollFrames = 500 / audio::kFrameMs;
struct Result {
  bool start{false};
  std::array<const audio::VoiceFrame16k*, kPreRollFrames> frames{};
  std::size_t count{0};
  bool end{false};
};
// 仅在应用Listening/Speaking时调用；开始监听时reset，END后停止update。
// PCM借用到下一次update/reset。调用方先处理断点，依次START、frames、END。
void reset() noexcept;
Result update(const audio::CaptureFrame& frame) noexcept;
}  // namespace boompi::speech
