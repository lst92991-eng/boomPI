#include "boompi/audio/speech.h"

#include <algorithm>

namespace boompi::speech {
namespace {
constexpr unsigned kStartFrames = 300 / audio::kFrameMs;
constexpr unsigned kEndFrames = 700 / audio::kFrameMs;
constexpr unsigned kMaxFrames = 60000 / audio::kFrameMs;
std::array<audio::VoiceFrame16k, kPreRollFrames> history;
std::size_t next{0}, stored{0};
unsigned voice_frames{0}, quiet_frames{0}, utterance_frames{0};

}  // namespace

void reset() noexcept {
  next = stored = 0;
  voice_frames = quiet_frames = utterance_frames = 0;
}

Result update(const audio::CaptureFrame& frame) noexcept {
  Result result;
  if (utterance_frames != 0) {
    // 已确认的语音直接交付，不再经过历史环，也不再次检查开口电平。
    quiet_frames = frame.vad_now ? 0 : quiet_frames + 1;
    result.frames[0] = &frame.pcm;
    result.count = 1;
    result.end = ++utterance_frames >= kMaxFrames || quiet_frames >= kEndFrames;
    return result;
  }
  history[next] = frame.pcm;
  next = (next + 1) % kPreRollFrames;
  stored = std::min(stored + 1, kPreRollFrames);
  voice_frames = frame.vad_now ? voice_frames + 1 : 0;
  if (voice_frames == kStartFrames) {
    // 普通提问、追问和插话只确认这一次；当前帧已经包含在前滚中。
    result.start = true;
    result.count = stored;
    utterance_frames = static_cast<unsigned>(result.count);
    for (std::size_t i = 0; i < result.count; ++i) {
      result.frames[i] = &history[(next + kPreRollFrames - stored + i) % kPreRollFrames];
    }
  }
  return result;
}
}  // namespace boompi::speech
