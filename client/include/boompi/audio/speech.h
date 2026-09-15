#pragma once
#include "boompi/audio/audio_frames.h"

namespace boompi::speech {
inline constexpr std::size_t kPreRollFrames = 500 / audio::kFrameMs;
struct Result {
  bool start{false};
  std::array<const audio::VoiceFrame16k*, kPreRollFrames> frames{};
  std::size_t count{0};
  bool end{false};
  bool hold_playback{false};  // 候选只请求短暂停播，确认后才start。
};
// 仅在应用Listening/Speaking时调用；开始监听时reset，END后停止update。
// PCM借用到下一次update/reset。调用方先处理断点，依次START、frames、END。
void reset() noexcept;
// speaking指当前有声回复；自然尾播仍在本模块短暂保留回声上下文。
Result update(const audio::CaptureFrame& frame, bool speaking = false) noexcept;
}  // namespace boompi::speech
