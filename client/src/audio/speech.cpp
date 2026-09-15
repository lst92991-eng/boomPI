#include "boompi/audio/speech.h"

#include <algorithm>
#include <cstdio>

#include "audio_convert.h"
#include "board_voice_profile.h"
#include "frame_queue.h"
#include "vad.h"
#include "wake.h"

namespace boompi::speech {
namespace {
using audio::CaptureFrame;
using audio::VoiceFrameContract;
constexpr std::size_t kPreRollFrames = VoiceFrameContract::FramesForMs(500);
constexpr std::size_t kFollowUpFrames = VoiceFrameContract::FramesForMs(400);
constexpr std::size_t kCandidateFrames = VoiceFrameContract::FramesForMs(120);
constexpr std::size_t kLowReferenceFrames = VoiceFrameContract::FramesForMs(60);
constexpr std::size_t kReferenceWaitFrames = VoiceFrameContract::FramesForMs(300);
constexpr std::size_t kEchoTailFrames = VoiceFrameContract::FramesForMs(60);
constexpr std::size_t kConfirmFrames = VoiceFrameContract::FramesForMs(60);
constexpr std::size_t kRetryFrames = VoiceFrameContract::FramesForMs(300);
constexpr std::size_t kHistoryFrames = 32;
static_assert(kCandidateFrames + kReferenceWaitFrames + kEchoTailFrames + kConfirmFrames <=
              kHistoryFrames);

enum class Input { Idle, Listening, FollowUp, Recording };
enum class Probe { Candidate, ReferenceLow, EchoTail, Confirm };
Input input{Input::Idle};
Probe probe{Probe::Candidate};
std::size_t utterance_frames{0};
audio::FrameQueue<CaptureFrame, kHistoryFrames> history;
unsigned follow_up_frames{0}, probe_frames{0}, low_reference_frames{0}, cooldown_frames{0};

// AEC保护是业务准入，不属于WebRTC库；一个阶段替代原来的四个组合bool。
enum class Gate { Off, Reference, Warmup, Silent, Tail };
Gate gate{Gate::Off};
unsigned gate_frames{0}, voice_frames{0}, quiet_frames{0};

bool classify(CaptureFrame& frame, const playback::Observation& output) {
  frame.voice_dbfs = audio_convert::ac_rms_dbfs(frame.pcm);
  frame.vad_now = frame.vad_now &&
                  (voice_frames == 6 || frame.input_dbfs >= audio::board::kSpeechAdmissionDbfs);
  frame.vad_started = frame.vad_ended = false;
  if (frame.vad_now) {
    quiet_frames = 0;
    frame.vad_started = voice_frames == 5;
    voice_frames = std::min(voice_frames + 1, 6U);
  } else if (voice_frames < 6) {
    voice_frames = 0;
  } else if (++quiet_frames == 35) {
    voice_frames = quiet_frames = 0;
    frame.vad_ended = true;
  }
  bool suppress = false;
  if (output.end != playback::End::None) {
    gate = output.end == playback::End::Natural ? Gate::Tail : Gate::Off;
    gate_frames = gate == Gate::Tail ? 15 : 0;
  } else if (gate == Gate::Silent && output.output_audible) {
    gate = Gate::Reference;
    gate_frames = 0;
    suppress = true;
  } else if (gate == Gate::Reference) {
    suppress = true;
    if (output.render_started && !output.output_audible) {
      gate = Gate::Silent;
      suppress = false;
    } else if (output.render_started && frame.reference_active) {
      gate = Gate::Warmup;
      gate_frames = 30;
    } else if (output.render_started && gate_frames < 50 && ++gate_frames == 50) {
      std::fprintf(stderr, "boompi: AEC reference absent for 1000 ms of playback\n");
    }
    // 缺参考时保持关闭准入；不能以等待超时自动放行回声。
  }
  if (gate == Gate::Warmup || gate == Gate::Tail) {
    suppress = true;
    if (--gate_frames == 0) {
      gate = Gate::Off;
      voice_frames = quiet_frames = 0;
      if (!vad::reset()) {
        return false;
      }
    }
  }
  frame.near_voice = !suppress && frame.vad_now;
  return true;
}

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
  utterance_frames = result.count;
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
  utterance_frames = 0;
  history.Clear();
  follow_up_frames = cooldown_frames = 0;
  reset_probe();
  gate = Gate::Off;
  voice_frames = quiet_frames = gate_frames = 0;
}

bool listen(ListenMode mode) noexcept {
  // 重置语句准入但保留自然播放尾音保护。
  const auto saved_gate = gate;
  const auto saved_frames = gate_frames;
  reset();
  gate = saved_gate;
  gate_frames = saved_frames;
  input = mode == ListenMode::Wake ? Input::Listening : Input::FollowUp;
  return wake::reset() && vad::reset();
}
void reply_started() noexcept {
  reset();
  gate = Gate::Reference;
  gate_frames = 0;
}

Result update(CaptureFrame& frame, bool speaking,
              const playback::Observation& output) noexcept {
  if (frame.discontinuity) {
    reset();
    gate = speaking ? Gate::Reference : Gate::Off;
    Result result;
    result.decision = Decision::Fault;
    return result;
  }
  if (!classify(frame, output)) {
    Result result;
    result.decision = Decision::Fault;
    result.error = "playback VAD reset failed";
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
      ++utterance_frames;
      result.end =
          frame.vad_ended || utterance_frames >= VoiceFrameContract::FramesForMs(60000);
      if (result.end) {
        input = Input::Idle;
      }
      break;
  }
  return result;
}

}  // namespace boompi::speech
