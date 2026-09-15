#include "boompi/audio/speech.h"

#include "board_voice_profile.h"
#include "frame_queue.h"

namespace boompi::speech {
namespace {
using audio::CaptureFrame;
using audio::VoiceFrameContract;
constexpr std::size_t kPreRollFrames = VoiceFrameContract::FramesForMs(500);
constexpr unsigned kFollowUpFrames = VoiceFrameContract::FramesForMs(400);
constexpr unsigned kCandidateFrames = VoiceFrameContract::FramesForMs(120);
constexpr unsigned kLowReferenceFrames = VoiceFrameContract::FramesForMs(60);
constexpr unsigned kReferenceWaitFrames = VoiceFrameContract::FramesForMs(300);
constexpr unsigned kEchoTailFrames = VoiceFrameContract::FramesForMs(60);
constexpr unsigned kConfirmFrames = VoiceFrameContract::FramesForMs(60);
constexpr unsigned kRetryFrames = VoiceFrameContract::FramesForMs(300);
constexpr std::size_t kHistoryFrames = 32;
static_assert(kCandidateFrames + kReferenceWaitFrames + kEchoTailFrames + kConfirmFrames <=
              kHistoryFrames);

enum class Input { Idle, Listening, FollowUp, Recording };
enum class Probe { Candidate, ReferenceLow, EchoTail, Confirm };
Input input{Input::Idle};
Probe probe{Probe::Candidate};
audio::FrameQueue<CaptureFrame, kHistoryFrames> history;
unsigned follow_up_frames{0}, probe_frames{0}, low_reference_frames{0}, cooldown_frames{0};

void reset_probe() noexcept {
  probe = Probe::Candidate;
  probe_frames = low_reference_frames = 0;
}

void keep_newest(std::size_t count) noexcept {
  while (history.Size() > count) {
    history.Pop();
  }
}

void remember(const CaptureFrame& frame, std::size_t limit) noexcept {
  keep_newest(limit - 1);
  history.Push(frame);
}

// 只返回现有环中的地址。当前帧已经在环内，不再额外实时发送一次。
Result admit(Decision decision) noexcept {
  Result result;
  result.decision = decision;
  input = Input::Recording;
  for (std::size_t i = 0; i < history.Size(); ++i) {
    result.frames[result.count++] = &history[i];
    if (history[i].vad_ended) {
      result.end = true;
      input = Input::Idle;
      break;
    }
  }
  return result;
}

bool near_speech(const CaptureFrame& frame) noexcept {
  return frame.near_voice && frame.voice_dbfs >= audio::board::kBargeVoiceDbfs;
}

void reject_probe() noexcept {
  reset_probe();
  cooldown_frames = kRetryFrames;
}

Result check_barge(const CaptureFrame& frame) noexcept {
  remember(frame, probe == Probe::Candidate ? kPreRollFrames : kHistoryFrames);
  switch (probe) {
    case Probe::Candidate:
      if (cooldown_frames != 0) {
        --cooldown_frames;
      } else if (!near_speech(frame)) {
        probe_frames = 0;
      } else if (++probe_frames >= kCandidateFrames) {
        keep_newest(kCandidateFrames);
        probe_frames = low_reference_frames = 0;
        probe = Probe::ReferenceLow;
      }
      break;
    case Probe::ReferenceLow:
      if (++probe_frames > kReferenceWaitFrames) {
        reject_probe();
      } else {
        low_reference_frames = frame.reference_active ? 0 : low_reference_frames + 1;
        if (low_reference_frames >= kLowReferenceFrames) {
          probe_frames = 0;
          probe = Probe::EchoTail;
        }
      }
      break;
    case Probe::EchoTail:
      if (frame.reference_active) {
        reject_probe();
      } else if (++probe_frames >= kEchoTailFrames) {
        probe_frames = 0;
        probe = Probe::Confirm;
      }
      break;
    case Probe::Confirm:
      if (frame.reference_active || !near_speech(frame)) {
        reject_probe();
      } else if (++probe_frames >= kConfirmFrames) {
        reset_probe();
        cooldown_frames = 0;
        return admit(Decision::Barge);
      }
      break;
  }
  Result result;
  // 静音后仍要等Mode1参考下降，再等房间尾音，不能VAD命中就取消旧回答。
  result.playback_scale = probe == Probe::Candidate ? 1.0F : 0.0F;
  return result;
}
}  // namespace

void reset() noexcept {
  input = Input::Idle;
  history.Clear();
  follow_up_frames = cooldown_frames = 0;
  reset_probe();
}

void listen(ListenMode mode) noexcept {
  reset();
  input = mode == ListenMode::Wake ? Input::Listening : Input::FollowUp;
}

Result update(const CaptureFrame& frame, bool speaking) noexcept {
  if (frame.discontinuity) {
    reset();
    Result result;
    result.decision = Decision::Fault;
    return result;
  }
  if (speaking) {
    return check_barge(frame);
  }

  Result result;
  switch (input) {
    case Input::Idle:
      result.decision = frame.wake ? Decision::Wake : Decision::None;
      break;
    case Input::Listening:
      remember(frame, kPreRollFrames);
      if (frame.vad_started) {
        return admit(Decision::Start);
      }
      break;
    case Input::FollowUp:
      // 未准入的短句结束后丢弃旧END，避免它截断下一次真正的追问。
      if (frame.vad_ended) {
        follow_up_frames = 0;
        history.Clear();
        break;
      }
      remember(frame, kPreRollFrames);
      if (!frame.near_voice) {
        follow_up_frames = 0;
      } else if (++follow_up_frames >= kFollowUpFrames) {
        return admit(Decision::Start);
      }
      break;
    case Input::Recording:
      history.Clear();
      result.decision = Decision::Pcm;
      result.frames[0] = &frame;
      result.count = 1;
      result.end = frame.vad_ended;
      if (result.end) {
        input = Input::Idle;
      }
      break;
  }
  return result;
}

}  // namespace boompi::speech
