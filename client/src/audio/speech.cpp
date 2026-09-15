#include "boompi/audio/speech.h"

#include <algorithm>
#include <cstdio>

#include "board_voice_profile.h"

namespace boompi::speech {
namespace {
constexpr unsigned kStartFrames = 300 / audio::kFrameMs;
constexpr unsigned kEndFrames = 700 / audio::kFrameMs;
constexpr unsigned kMaxFrames = 60000 / audio::kFrameMs;
constexpr unsigned kCandidateFrames = audio::board::kBargeCandidateMs / audio::kFrameMs;
constexpr unsigned kProbeFrames = audio::board::kBargeProbeMs / audio::kFrameMs;
constexpr unsigned kSettleFrames = audio::board::kBargeSettleMs / audio::kFrameMs;
constexpr unsigned kConfirmFrames = audio::board::kBargeConfirmMs / audio::kFrameMs;
static_assert(kCandidateFrames + kProbeFrames <= kPreRollFrames,
              "probe must retain the entire candidate in the existing pre-roll");
std::array<audio::VoiceFrame16k, kPreRollFrames> history;
std::size_t next{0}, stored{0};
unsigned voice_frames{0}, quiet_frames{0}, utterance_frames{0};
unsigned probe_frames{0}, reference_quiet_frames{0}, retry_frames{0}, tail_frames{0};
bool probing{false}, reference_seen{false};

bool near_voice(const audio::CaptureFrame& frame) noexcept {
  if (!frame.vad_now) {
    return false;
  }
  // 去直流后的RMS平方；VAD保持位或直流偏置本身不能通过静音后复核。
  double sum = 0, squares = 0;
  for (const double sample : frame.pcm) {
    sum += sample;
    squares += sample * sample;
  }
  constexpr double kSamples = static_cast<double>(audio::kVoiceFrameSamples);
  const double mean = sum / kSamples;
  return squares / kSamples - mean * mean >=
         audio::board::kBargeMinRms * audio::board::kBargeMinRms;
}
}  // namespace

void reset() noexcept {
  next = stored = 0;
  voice_frames = quiet_frames = utterance_frames = 0;
  probe_frames = reference_quiet_frames = retry_frames = tail_frames = 0;
  probing = reference_seen = false;
}

Result update(const audio::CaptureFrame& frame, bool speaking) noexcept {
  Result result;
  if (frame.discontinuity) {
    reset();
    return result;
  }
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
  reference_seen = reference_seen || frame.reference_active;
  if (speaking || frame.reference_active) {
    tail_frames = audio::board::kPlaybackTailMs / audio::kFrameMs;
  } else if (tail_frames != 0) {
    --tail_frames;
  }
  if (probing) {
    ++probe_frames;
    // 先见播放线程实际送静音，再等低参考/尾音；不把软件hold请求当成声学事实。
    reference_quiet_frames = !frame.reference_active && (frame.playback_held || !speaking)
                                 ? reference_quiet_frames + 1
                                 : 0;
    voice_frames = reference_quiet_frames > kSettleFrames && near_voice(frame)
                       ? voice_frames + 1
                       : 0;
    if (voice_frames >= kConfirmFrames) {
      probing = false;
      result.start = true;
      std::fprintf(stderr, "boompi: barge confirmed\n");
    } else if (probe_frames >= kProbeFrames) {
      // 丢掉被拒绝的候选；恢复旧回答，不创建空轮次，不把旧END或回声带给追问。
      probing = false;
      next = stored = voice_frames = reference_quiet_frames = 0;
      retry_frames = audio::board::kBargeRetryMs / audio::kFrameMs;
      std::fprintf(stderr, "boompi: barge rejected; reference=%d\n", frame.reference_active);
    }
  } else if (retry_frames != 0) {
    --retry_frames;
    voice_frames = 0;
  } else if (tail_frames != 0) {
    // 有声播放缺参考时不放行；没有固定600ms禁插话窗口。
    voice_frames = reference_seen && near_voice(frame) ? voice_frames + 1 : 0;
    if (voice_frames >= kCandidateFrames) {
      probing = true;
      probe_frames = reference_quiet_frames = voice_frames = 0;
      std::fprintf(stderr, "boompi: barge probe\n");
    }
  } else {
    voice_frames = frame.vad_now ? voice_frames + 1 : 0;
    result.start = voice_frames >= kStartFrames;
  }
  result.hold_playback = probing;
  if (result.start) {
    // 确认之前不进入上传态；候选、复核、当前帧共用这一个500ms环。
    result.count = stored;
    utterance_frames = static_cast<unsigned>(result.count);
    for (std::size_t i = 0; i < result.count; ++i) {
      result.frames[i] = &history[(next + kPreRollFrames - stored + i) % kPreRollFrames];
    }
  }
  return result;
}
}  // namespace boompi::speech
