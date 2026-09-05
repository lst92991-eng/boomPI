/**
 * @file voice_audio.cpp
 * @brief 将固定帧音频、VAD、pre-roll、播放和打断收敛成 application 语义事件。
 */
#include "boompi/audio/voice_audio.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <new>
#include <string>
#include <thread>

#include "boompi/audio/audio_engine.h"

namespace boompi::audio {
namespace {

constexpr std::size_t kPreRollFrames =
    VoiceFrameContract::FramesForMs(500U);
constexpr std::size_t kBargeHistoryFrames = 32U;
constexpr unsigned kVadStartFrames = static_cast<unsigned>(
    VoiceFrameContract::FramesForMs(120U));
constexpr unsigned kFollowUpFrames = static_cast<unsigned>(
    VoiceFrameContract::FramesForMs(400U));
constexpr unsigned kBargeCandidateFrames = kVadStartFrames;
constexpr unsigned kBargeReferenceLowFrames = static_cast<unsigned>(
    VoiceFrameContract::FramesForMs(60U));
constexpr unsigned kBargeReferenceWaitFrames = static_cast<unsigned>(
    VoiceFrameContract::FramesForMs(300U));
constexpr unsigned kBargeEchoClearFrames = static_cast<unsigned>(
    VoiceFrameContract::FramesForMs(60U));
constexpr unsigned kBargeConfirmFrames = static_cast<unsigned>(
    VoiceFrameContract::FramesForMs(60U));
constexpr unsigned kBargeRetryCooldownFrames = static_cast<unsigned>(
    VoiceFrameContract::FramesForMs(300U));
constexpr std::size_t kMaximumBargeProbeFrames =
    kBargeCandidateFrames + kBargeReferenceWaitFrames +
    kBargeEchoClearFrames + kBargeConfirmFrames;
constexpr std::size_t kEventSlots = 64U;
constexpr unsigned kCaptureDrainPerPoll = 8U;
constexpr auto kPlaybackStopWait = std::chrono::milliseconds(60);

static_assert(kPreRollFrames <= kBargeHistoryFrames,
              "barge history must also hold normal pre-roll");
static_assert(kMaximumBargeProbeFrames <= kBargeHistoryFrames,
              "barge history must hold one complete acoustic probe");
static_assert(kVoiceFrameSamples == 320U && kTtsFrameSamples == 480U,
              "VoiceAudio public and wire frame contracts changed");

}  // namespace

struct VoiceAudio::Impl final {
  enum class InputState : std::uint8_t {
    kIdle,
    kListening,
    kFollowingUp,
    kCapturing,
  };
  enum class BargeStage : std::uint8_t {
    kIdle,
    kWaitReferenceLow,
    kClear,
    kVerify,
  };

  AudioEngine engine{};
  std::array<AudioEvent, kEventSlots> events{};
  std::size_t event_head{0U}, event_count{0U};
  std::array<CaptureFrame, kBargeHistoryFrames> history{};
  std::size_t history_head{0U}, history_count{0U};
  InputState input_state{InputState::kIdle};
  BargeStage barge_stage{BargeStage::kIdle};
  unsigned follow_up_frames{0U};
  unsigned barge_stage_frames{0U};
  unsigned barge_reference_low_frames{0U};
  unsigned barge_cooldown_frames{0U};
  std::uint32_t playback_generation{0U};
  bool playback_ending{false};
  bool open{false};
  bool fatal{false};
  std::array<char, 192U> error{};

  void SetError(const char* const why) noexcept {
    std::snprintf(error.data(), error.size(), "%s", why);
  }

  void ClearError() noexcept { error.fill('\0'); }

  void ClearEvents() noexcept {
    for (auto& event : events) event = {};
    event_head = event_count = 0U;
  }

  void ClearHistory() noexcept {
    for (auto& frame : history) frame = {};
    history_head = history_count = 0U;
  }

  bool PushEvent(const AudioEvent& event) noexcept {
    if (event_count == events.size()) return false;
    events[(event_head + event_count) % events.size()] = event;
    ++event_count;
    return true;
  }

  bool PopEvent(AudioEvent* const event) noexcept {
    if (event == nullptr || event_count == 0U) return false;
    *event = events[event_head];
    events[event_head] = {};
    event_head = (event_head + 1U) % events.size();
    --event_count;
    return true;
  }

  void ResetBargeProbe() noexcept {
    barge_stage = BargeStage::kIdle;
    barge_stage_frames = 0U;
    barge_reference_low_frames = 0U;
    engine.SetPlaybackScale(1.0F);
  }

  void DisarmInput() noexcept {
    input_state = InputState::kIdle;
    follow_up_frames = 0U;
    ClearHistory();
  }

  void ReportFault(const char* const why, const bool is_fatal,
                   const std::uint32_t generation = 0U) noexcept {
    SetError(why);
    fatal = fatal || is_fatal;
    DisarmInput();
    ResetBargeProbe();
    ClearEvents();
    AudioEvent event{};
    event.kind = AudioEventKind::Fault;
    event.generation = generation != 0U ? generation : playback_generation;
    static_cast<void>(PushEvent(event));
  }

  void SaveHistory(const CaptureFrame& frame, const std::size_t limit) noexcept {
    const std::size_t bounded_limit = std::min(limit, history.size());
    if (bounded_limit == 0U) return;
    if (history_count < bounded_limit) {
      history[(history_head + history_count) % history.size()] = frame;
      ++history_count;
      return;
    }
    history[(history_head + history_count) % history.size()] = frame;
    history_head = (history_head + 1U) % history.size();
  }

  void KeepNewestHistory(const std::size_t count) noexcept {
    const std::size_t keep = std::min(count, history_count);
    history_head = (history_head + history_count - keep) % history.size();
    history_count = keep;
  }

  AudioEvent PcmEvent(const CaptureFrame& frame, const bool end) const noexcept {
    AudioEvent event{};
    event.kind = AudioEventKind::Pcm;
    event.pcm = frame.pcm;
    event.sequence = frame.sequence;
    event.timestamp_us = frame.timestamp_us;
    event.end = end;
    return event;
  }

  bool QueueBufferedInput(const AudioEventKind start_kind,
                          const std::uint32_t generation) noexcept {
    std::size_t send_count = history_count;
    bool ended = false;
    for (std::size_t offset = 0U; offset < history_count; ++offset) {
      const CaptureFrame& frame =
          history[(history_head + offset) % history.size()];
      if (frame.vad_ended) {
        send_count = offset + 1U;
        ended = true;
        break;
      }
    }
    const std::size_t required = 1U + send_count + (ended ? 1U : 0U);
    if (required > events.size() - event_count) {
      ReportFault("voice event queue overflow", false);
      return false;
    }
    AudioEvent start{};
    start.kind = start_kind;
    start.generation = generation;
    static_cast<void>(PushEvent(start));
    for (std::size_t offset = 0U; offset < send_count; ++offset) {
      const CaptureFrame& frame =
          history[(history_head + offset) % history.size()];
      static_cast<void>(PushEvent(PcmEvent(frame, ended && offset + 1U == send_count)));
    }
    ClearHistory();
    if (ended) {
      AudioEvent speech_end{};
      speech_end.kind = AudioEventKind::SpeechEnd;
      static_cast<void>(PushEvent(speech_end));
      input_state = InputState::kIdle;
    } else {
      input_state = InputState::kCapturing;
    }
    return true;
  }

  bool IsBargeVoice(const CaptureFrame& frame) const noexcept {
    return frame.near_voice &&
           frame.voice_dbfs >= kBoardVoiceProfile.barge_voice_dbfs;
  }

  void RejectBarge() noexcept {
    ResetBargeProbe();
    barge_cooldown_frames = kBargeRetryCooldownFrames;
  }

  void ConfirmBarge() noexcept {
    const std::uint32_t interrupted_generation = playback_generation;
    engine.DropPlayback();
    playback_generation = 0U;
    playback_ending = false;
    ResetBargeProbe();
    barge_cooldown_frames = 0U;
    static_cast<void>(QueueBufferedInput(AudioEventKind::Barge,
                                        interrupted_generation));
  }

  void HandleBarge(const CaptureFrame& frame) noexcept {
    switch (barge_stage) {
      case BargeStage::kIdle:
        if (barge_cooldown_frames != 0U) {
          --barge_cooldown_frames;
          return;
        }
        if (!IsBargeVoice(frame)) {
          barge_stage_frames = 0U;
          return;
        }
        if (++barge_stage_frames < kBargeCandidateFrames) return;
        KeepNewestHistory(kBargeCandidateFrames);
        barge_stage_frames = 0U;
        barge_reference_low_frames = 0U;
        barge_stage = BargeStage::kWaitReferenceLow;
        engine.SetPlaybackScale(0.0F);
        return;

      case BargeStage::kWaitReferenceLow:
        if (++barge_stage_frames > kBargeReferenceWaitFrames) {
          RejectBarge();
          return;
        }
        barge_reference_low_frames = frame.reference_active
                                         ? 0U
                                         : barge_reference_low_frames + 1U;
        if (barge_reference_low_frames < kBargeReferenceLowFrames) return;
        barge_stage_frames = 0U;
        barge_stage = BargeStage::kClear;
        return;

      case BargeStage::kClear:
        if (frame.reference_active) {
          RejectBarge();
          return;
        }
        if (++barge_stage_frames < kBargeEchoClearFrames) return;
        barge_stage_frames = 0U;
        barge_stage = BargeStage::kVerify;
        return;

      case BargeStage::kVerify:
        if (frame.reference_active || !IsBargeVoice(frame)) {
          RejectBarge();
          return;
        }
        if (++barge_stage_frames >= kBargeConfirmFrames) ConfirmBarge();
        return;
    }
  }

  void FinishInput(const CaptureFrame& frame) noexcept {
    if (!PushEvent(PcmEvent(frame, true))) {
      ReportFault("voice event queue overflow", false);
      return;
    }
    AudioEvent ended{};
    ended.kind = AudioEventKind::SpeechEnd;
    if (!PushEvent(ended)) {
      ReportFault("voice event queue overflow", false);
      return;
    }
    input_state = InputState::kIdle;
    follow_up_frames = 0U;
    ClearHistory();
  }

  void HandleCapture(const CaptureFrame& frame) noexcept {
    if (frame.discontinuity) {
      ReportFault(frame.actor_overrun ? "voice capture queue overrun"
                                      : "voice capture discontinuity",
                  false);
      return;
    }

    if (playback_generation != 0U) {
      const std::size_t history_limit =
          barge_stage == BargeStage::kIdle ? kPreRollFrames
                                           : kBargeHistoryFrames;
      SaveHistory(frame, history_limit);
      HandleBarge(frame);
      return;
    }

    switch (input_state) {
      case InputState::kIdle:
        if (frame.wake) {
          AudioEvent wake{};
          wake.kind = AudioEventKind::Wake;
          if (!PushEvent(wake)) ReportFault("voice event queue overflow", false);
        }
        return;

      case InputState::kListening:
        SaveHistory(frame, kPreRollFrames);
        if (frame.vad_started)
          static_cast<void>(QueueBufferedInput(AudioEventKind::SpeechStart, 0U));
        return;

      case InputState::kFollowingUp:
        // 未达到 400 ms 追问准入的短句已经结束后，旧 END 不能留在下一句话的
        // pre-roll 中；否则真正问题刚被准入就会被旧边界提前截断。
        if (frame.vad_ended) {
          follow_up_frames = 0U;
          ClearHistory();
          return;
        }
        SaveHistory(frame, kPreRollFrames);
        if (!frame.near_voice) {
          follow_up_frames = 0U;
        } else if (++follow_up_frames >= kFollowUpFrames) {
          static_cast<void>(QueueBufferedInput(AudioEventKind::SpeechStart, 0U));
        }
        return;

      case InputState::kCapturing:
        if (frame.vad_ended) {
          FinishInput(frame);
        } else if (!PushEvent(PcmEvent(frame, false))) {
          ReportFault("voice event queue overflow", false);
        }
        return;
    }
  }

  void CheckPlaybackCompletion() noexcept {
    if (playback_generation == 0U || !engine.playback_done()) return;
    const std::uint32_t generation = playback_generation;
    playback_generation = 0U;
    const bool expected = playback_ending;
    playback_ending = false;
    ResetBargeProbe();
    barge_cooldown_frames = 0U;
    ClearHistory();
    if (engine.playback_failed() || !expected) {
      ReportFault("voice playback failed", true, generation);
      return;
    }
    AudioEvent done{};
    done.kind = AudioEventKind::PlaybackDone;
    done.generation = generation;
    if (!PushEvent(done)) ReportFault("voice event queue overflow", false);
  }

  CaptureResult CaptureOnce(const std::chrono::milliseconds timeout) noexcept {
    CaptureFrame frame{};
    const CaptureResult result = engine.Capture(&frame, timeout);
    if (result == CaptureResult::kFrame) {
      CheckPlaybackCompletion();
      if (!fatal) HandleCapture(frame);
    } else if (result == CaptureResult::kFailed) {
      ReportFault("voice capture failed", true);
    }
    return result;
  }

  void DrainReadyCapture() noexcept {
    for (unsigned drained = 0U; drained < kCaptureDrainPerPoll && !fatal;
         ++drained) {
      if (CaptureOnce(std::chrono::milliseconds::zero()) !=
          CaptureResult::kFrame)
        break;
    }
  }

  bool WaitForPlaybackStop() noexcept {
    const auto deadline = std::chrono::steady_clock::now() + kPlaybackStopWait;
    while (!engine.playback_done()) {
      DrainReadyCapture();
      if (fatal || std::chrono::steady_clock::now() >= deadline) return false;
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
  }
};

VoiceAudio::~VoiceAudio() noexcept {
  Close();
  delete impl_;
}

bool VoiceAudio::Open(const std::uint8_t volume) {
  if (impl_ == nullptr) impl_ = new (std::nothrow) Impl;
  if (impl_ == nullptr) return false;
  Close();
  impl_->ClearEvents();
  impl_->ClearHistory();
  impl_->fatal = false;
  impl_->ClearError();
  AudioEngineConfig config{};
  config.playback_gain = static_cast<float>(std::min<std::uint8_t>(volume, 100U)) /
                         100.0F;
  if (!impl_->engine.Open(config)) {
    impl_->fatal = true;
    const std::string error = impl_->engine.last_error();
    impl_->SetError(error.c_str());
    return false;
  }
  impl_->open = true;
  return true;
}

bool VoiceAudio::Poll(AudioEvent* const event,
                      const std::chrono::milliseconds timeout) {
  if (impl_ == nullptr || !impl_->open || event == nullptr ||
      timeout < std::chrono::milliseconds::zero())
    return false;

  impl_->CheckPlaybackCompletion();
  if (impl_->event_count != 0U) {
    impl_->DrainReadyCapture();
    return impl_->PopEvent(event);
  }

  const CaptureResult result = impl_->CaptureOnce(timeout);
  if (result == CaptureResult::kFrame) impl_->DrainReadyCapture();
  impl_->CheckPlaybackCompletion();
  return impl_->PopEvent(event);
}

bool VoiceAudio::Listen(const bool follow_up) {
  if (impl_ == nullptr || !impl_->open || impl_->fatal ||
      impl_->playback_generation != 0U)
    return false;
  impl_->ClearEvents();
  impl_->DisarmInput();
  impl_->ResetBargeProbe();
  impl_->barge_cooldown_frames = 0U;
  if (!impl_->engine.ResetListener()) {
    impl_->ReportFault("voice listener reset failed", true);
    return false;
  }
  impl_->ClearError();
  impl_->input_state = follow_up ? Impl::InputState::kFollowingUp
                                 : Impl::InputState::kListening;
  return true;
}

bool VoiceAudio::Play(const std::uint32_t generation,
                      const std::uint8_t* const pcm,
                      const std::size_t bytes,
                      const std::uint32_t sequence,
                      const bool start,
                      const bool end) {
  if (impl_ == nullptr || !impl_->open || impl_->fatal || generation == 0U ||
      pcm == nullptr || bytes == 0U || bytes > kTtsFrameSamples * 2U ||
      (bytes & 1U) != 0U || (!end && bytes != kTtsFrameSamples * 2U) ||
      (start != (sequence == 0U)))
    return false;

  if (start) {
    if (impl_->playback_generation != 0U || !impl_->WaitForPlaybackStop() ||
        !impl_->engine.BeginPlayback()) {
      impl_->SetError("voice playback start failed");
      return false;
    }
    impl_->playback_generation = generation;
    impl_->playback_ending = false;
    impl_->ResetBargeProbe();
    impl_->barge_cooldown_frames = 0U;
    impl_->ClearHistory();
  } else if (generation != impl_->playback_generation ||
             impl_->playback_ending) {
    return false;
  }

  const QueueTtsResult queued =
      impl_->engine.QueueTts24k(pcm, bytes, sequence);
  if (queued != QueueTtsResult::kQueued) {
    impl_->SetError("voice playback queue rejected PCM");
    impl_->engine.DropPlayback();
    impl_->playback_generation = 0U;
    impl_->playback_ending = false;
    impl_->ResetBargeProbe();
    impl_->ClearHistory();
    return false;
  }
  if (end) {
    if (!impl_->engine.EndPlayback()) {
      impl_->SetError("voice playback finish failed");
      impl_->engine.DropPlayback();
      impl_->playback_generation = 0U;
      impl_->playback_ending = false;
      impl_->ResetBargeProbe();
      impl_->ClearHistory();
      return false;
    }
    impl_->playback_ending = true;
  }
  impl_->ClearError();
  return true;
}

void VoiceAudio::StopPlayback() {
  if (impl_ == nullptr || !impl_->open) return;
  impl_->engine.DropPlayback();
  impl_->playback_generation = 0U;
  impl_->playback_ending = false;
  impl_->ResetBargeProbe();
  impl_->barge_cooldown_frames = 0U;
  impl_->ClearHistory();
}

void VoiceAudio::CancelInput() {
  if (impl_ == nullptr || !impl_->open) return;
  impl_->ClearEvents();
  impl_->DisarmInput();
  // CancelInput 只取消当前上行。若正在播放，下一帧仍会自动进入同一个 barge 探针。
  impl_->ResetBargeProbe();
  impl_->barge_cooldown_frames = 0U;
}

void VoiceAudio::SetVolume(const std::uint8_t volume) {
  if (impl_ == nullptr || !impl_->open) return;
  impl_->engine.SetPlaybackGain(
      static_cast<float>(std::min<std::uint8_t>(volume, 100U)) / 100.0F);
}

bool VoiceAudio::healthy() const {
  return impl_ != nullptr && impl_->open && !impl_->fatal;
}

std::string VoiceAudio::last_error() const {
  if (impl_ == nullptr) return "voice audio is not allocated";
  if (impl_->error[0] != '\0') return impl_->error.data();
  return impl_->engine.last_error();
}

void VoiceAudio::Close() noexcept {
  if (impl_ == nullptr) return;
  impl_->engine.Close();
  impl_->ClearEvents();
  impl_->ClearHistory();
  impl_->input_state = Impl::InputState::kIdle;
  impl_->barge_stage = Impl::BargeStage::kIdle;
  impl_->follow_up_frames = impl_->barge_stage_frames =
      impl_->barge_reference_low_frames = impl_->barge_cooldown_frames = 0U;
  impl_->playback_generation = 0U;
  impl_->playback_ending = false;
  impl_->open = false;
  impl_->fatal = false;
}

}  // namespace boompi::audio
