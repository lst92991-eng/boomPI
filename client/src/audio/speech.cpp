/** @file speech.cpp
 * @brief 主线程的语句策略：记录句首 → 确认开口 → 逐帧交付 → 确认句尾。
 *
 * 普通开口看连续 VAD；有声回复期间先检测候选，暂停播放后再用参考和人声复核。
 * history 只保存尚未确认的 500ms，开始上传后直接借用当前输入，不保存整句话。
 */
#include "boompi/audio/speech.h"

#include <algorithm>

#include "board_voice_profile.h"
#include "boompi/debug.h"

namespace boompi::speech {
namespace {
constexpr unsigned kStartFrames = 300 / audio::kFrameMs;
constexpr unsigned kEndFrames = 700 / audio::kFrameMs;
constexpr unsigned kMaxFrames = 60000 / audio::kFrameMs;
constexpr unsigned kCandidateFrames = audio::board::kBargeCandidateMs / audio::kFrameMs;
constexpr unsigned kProbeFrames = audio::board::kBargeProbeMs / audio::kFrameMs;
constexpr unsigned kSettleFrames = audio::board::kBargeSettleMs / audio::kFrameMs;
constexpr unsigned kConfirmFrames = audio::board::kBargeConfirmMs / audio::kFrameMs;
// 120ms 候选 + 380ms 复核正好由 500ms 前滚覆盖；调整时要一起核对，避免丢句首。
std::array<audio::VoiceFrame16k, kPreRollFrames> history;
// next 为下一写入位置，stored 为有效帧数；从 next-stored 起按时间顺序回放句首。
std::size_t next{0}, stored{0};
// utterance_frames 非零表示已确认本句，直到应用 END 后停止 update 或下一窗口 reset。
unsigned voice_frames{0}, quiet_frames{0}, utterance_frames{0};
unsigned probe_frames{0}, reference_quiet_frames{0}, retry_frames{0}, tail_frames{0};
bool probing{false}, reference_seen{false};

/** @brief 插话候选需同时满足 VAD 和交流能量；这只是准入条件，后面仍需停播复核。 */
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
  // 当前帧先入历史；确认成功时整段历史已含它，不能再单独发送一次当前帧。
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
    voice_frames =
        reference_quiet_frames > kSettleFrames && near_voice(frame) ? voice_frames + 1 : 0;
    if (voice_frames >= kConfirmFrames) {
      probing = false;
      result.start = true;
      debug::log.barge_confirmed();
    } else if (probe_frames >= kProbeFrames) {
      // 丢掉被拒绝的候选；恢复旧回答，不创建空轮次，不把旧END或回声带给追问。
      probing = false;
      next = stored = voice_frames = reference_quiet_frames = 0;
      retry_frames = audio::board::kBargeRetryMs / audio::kFrameMs;
      debug::log.barge_rejected(frame.reference_active);
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
      debug::log.barge_probe();
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
