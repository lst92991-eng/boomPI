#include "boompi/audio/vad_utterance_controller.h"

#include <limits>

namespace boompi::audio {
namespace {

constexpr VadControlIntentMask AddIntent(const VadControlIntentMask mask,
                                         const VadControlIntent intent) {
  return static_cast<VadControlIntentMask>(mask | ToMask(intent));
}

bool IsAcceptedContinuity(const FrameContinuityResult result) noexcept {
  return result == FrameContinuityResult::kAcceptedFirst ||
         result == FrameContinuityResult::kAcceptedContinuous;
}

}  // namespace

Status VadUtteranceController::Arm(const std::uint32_t epoch,
                                   const std::uint32_t stream_id) {
  FlushAndRetireGeneration();
  if (epoch == 0U || stream_id == 0U) {
    faulted_ = true;
    return Status::Error(StatusCode::kInvalidArgument,
                         "VAD generation identity must be non-zero");
  }
  if (highest_epoch_ != 0U && epoch <= highest_epoch_) {
    faulted_ = true;
    return Status::Error(
        StatusCode::kFailedPrecondition,
        "VAD Arm requires an epoch newer than every accepted epoch");
  }
  const Status status = continuity_gate_.Arm(epoch, stream_id);
  if (!status.ok()) {
    faulted_ = true;
    return status;
  }

  epoch_ = epoch;
  highest_epoch_ = epoch;
  stream_id_ = stream_id;
  armed_ = true;
  faulted_ = false;
  return Status::Ok();
}

void VadUtteranceController::Disarm() noexcept {
  FlushAndRetireGeneration();
}

Status VadUtteranceController::BeginListening(
    const std::uint32_t user_turn_id,
    const std::uint64_t command_timestamp_us) {
  if (!armed_ || faulted_) {
    return Status::Error(StatusCode::kFailedPrecondition,
                         "VAD controller is not armed");
  }
  if (user_turn_id == 0U) {
    return Status::Error(StatusCode::kInvalidArgument,
                         "VAD user turn id must be non-zero");
  }
  if (capture_phase_ != CapturePhase::kMonitoring || playback_active_ ||
      uplink_count_ != 0U) {
    return Status::Error(
        StatusCode::kFailedPrecondition,
        "VAD controller cannot begin a new listening turn");
  }
  if (command_timestamp_us >
      std::numeric_limits<std::uint64_t>::max() - kFirstSpeechTimeoutUs) {
    return Status::Error(StatusCode::kInvalidArgument,
                         "VAD listening deadline would overflow");
  }

  user_turn_id_ = user_turn_id;
  first_speech_deadline_us_ = command_timestamp_us + kFirstSpeechTimeoutUs;
  capture_phase_ = CapturePhase::kAwaitingFirstSpeech;
  return Status::Ok();
}

Status VadUtteranceController::BeginPlaybackBargeIn(
    const std::uint32_t response_turn_id,
    const std::uint32_t next_user_turn_id) {
  if (!armed_ || faulted_) {
    return Status::Error(StatusCode::kFailedPrecondition,
                         "VAD controller is not armed");
  }
  if (response_turn_id == 0U || next_user_turn_id == 0U ||
      response_turn_id == next_user_turn_id) {
    return Status::Error(
        StatusCode::kInvalidArgument,
        "VAD playback and next user turn ids must be distinct and non-zero");
  }
  if (capture_phase_ != CapturePhase::kMonitoring || playback_active_ ||
      uplink_count_ != 0U) {
    return Status::Error(StatusCode::kFailedPrecondition,
                         "VAD controller cannot begin playback monitoring");
  }

  playback_active_ = true;
  response_turn_id_ = response_turn_id;
  user_turn_id_ = next_user_turn_id;
  barge_in_speech_frames_ = 0U;
  duck_requested_ = false;
  return Status::Ok();
}

VadControllerResult VadUtteranceController::EndPlayback() noexcept {
  VadControllerResult result = MakeResult(VadControllerCode::kAccepted);
  if (playback_active_ && duck_requested_) {
    result.intents = AddIntent(result.intents,
                               VadControlIntent::kRestorePlayback);
  }
  ResetPlaybackState();
  if (capture_phase_ == CapturePhase::kMonitoring && uplink_count_ == 0U) {
    user_turn_id_ = 0U;
  }
  return result;
}

VadControllerResult VadUtteranceController::Cancel() noexcept {
  VadControllerResult result = MakeResult(VadControllerCode::kCancelled);
  result.end_reason = VadUtteranceEndReason::kCancelled;
  result.intents = ActiveCancellationIntents();
  FlushAndRetireGeneration();
  result.queued_uplink_frames = 0U;
  return result;
}

VadControllerResult VadUtteranceController::Process(
    const Mono16kFrame& frame, const VadObservation observation) noexcept {
  if (!armed_ || faulted_) {
    return LatchFault(VadUtteranceEndReason::kContinuityFault);
  }
  if (!frame.HasValidLength()) {
    return LatchFault(VadUtteranceEndReason::kInvalidInput);
  }

  const FrameContinuityResult continuity =
      continuity_gate_.CheckAndAdvance(frame.metadata);
  if (continuity == FrameContinuityResult::kEpochMismatch ||
      continuity == FrameContinuityResult::kStreamMismatch) {
    return MakeResult(VadControllerCode::kStaleFrameDropped);
  }
  if (!IsAcceptedContinuity(continuity)) {
    return LatchFault(VadUtteranceEndReason::kContinuityFault);
  }
  if (observation == VadObservation::kUnset ||
      (observation != VadObservation::kSilence &&
       observation != VadObservation::kSpeech)) {
    return LatchFault(VadUtteranceEndReason::kInvalidInput);
  }

  PushPreRoll(frame);

  if (capture_phase_ == CapturePhase::kAwaitingFirstSpeech) {
    if (frame.metadata.monotonic_timestamp_us >= first_speech_deadline_us_) {
      VadControllerResult result =
          MakeResult(VadControllerCode::kListeningTimedOut);
      result.end_reason = VadUtteranceEndReason::kFirstSpeechTimeout;
      result.intents = AddIntent(result.intents,
                                 VadControlIntent::kCancelUserTurn);
      FlushBuffersAndState();
      result.queued_uplink_frames = 0U;
      return result;
    }
    if (observation == VadObservation::kSpeech) {
      return StartUtterance(1U, false);
    }
    return MakeResult(VadControllerCode::kAccepted);
  }

  if (capture_phase_ == CapturePhase::kStreamingUtterance) {
    if (!EnqueueFrame(frame, user_turn_id_)) {
      return LatchFault(VadUtteranceEndReason::kOutputOverflow);
    }

    ++utterance_frame_count_;
    if (observation == VadObservation::kSpeech) {
      trailing_silence_frames_ = 0U;
    } else {
      ++trailing_silence_frames_;
    }

    if (trailing_silence_frames_ >= kVadTrailingSilenceFrameCount) {
      return FinishUtterance(VadUtteranceEndReason::kTrailingSilence);
    }
    if (utterance_frame_count_ >= kVadMaximumUtteranceFrameCount) {
      return FinishUtterance(VadUtteranceEndReason::kMaximumDuration);
    }
    return MakeResult(VadControllerCode::kAccepted);
  }

  if (playback_active_) {
    return ProcessPlaybackObservation(observation);
  }
  return MakeResult(VadControllerCode::kAccepted);
}

bool VadUtteranceController::TryPopUplinkFrame(
    Mono16kFrame* const output) noexcept {
  if (output == nullptr || uplink_count_ == 0U) {
    return false;
  }
  *output = uplink_queue_[uplink_read_index_];
  uplink_read_index_ =
      (uplink_read_index_ + 1U) % kVadUplinkQueueCapacity;
  --uplink_count_;
  if (uplink_count_ == 0U &&
      capture_phase_ == CapturePhase::kMonitoring && !playback_active_) {
    user_turn_id_ = 0U;
  }
  return true;
}

std::uint32_t VadUtteranceController::queued_uplink_frames() const noexcept {
  return static_cast<std::uint32_t>(uplink_count_);
}

bool VadUtteranceController::armed() const noexcept { return armed_; }

bool VadUtteranceController::faulted() const noexcept { return faulted_; }

VadControllerResult VadUtteranceController::MakeResult(
    const VadControllerCode code) const noexcept {
  VadControllerResult result{};
  result.code = code;
  result.epoch = epoch_;
  result.user_turn_id = user_turn_id_;
  result.response_turn_id = response_turn_id_;
  result.queued_uplink_frames =
      static_cast<std::uint32_t>(uplink_count_);
  return result;
}

VadControllerResult VadUtteranceController::LatchFault(
    const VadUtteranceEndReason reason) noexcept {
  VadControllerResult result = MakeResult(VadControllerCode::kFaulted);
  result.end_reason = reason;
  result.intents = ActiveCancellationIntents();
  FlushAndRetireGeneration();
  faulted_ = true;
  result.queued_uplink_frames = 0U;
  return result;
}

VadControllerResult VadUtteranceController::FinishUtterance(
    const VadUtteranceEndReason reason) noexcept {
  VadControllerResult result = MakeResult(VadControllerCode::kUtteranceEnded);
  result.end_reason = reason;
  result.intents = AddIntent(result.intents,
                             VadControlIntent::kEndUtterance);
  capture_phase_ = CapturePhase::kMonitoring;
  utterance_frame_count_ = 0U;
  trailing_silence_frames_ = 0U;
  first_speech_deadline_us_ = 0U;
  return result;
}

void VadUtteranceController::FlushAndRetireGeneration() noexcept {
  continuity_gate_.Disarm();
  FlushBuffersAndState();
  epoch_ = 0U;
  stream_id_ = 0U;
  armed_ = false;
  faulted_ = false;
}

void VadUtteranceController::FlushBuffersAndState() noexcept {
  pre_roll_write_index_ = 0U;
  pre_roll_count_ = 0U;
  uplink_read_index_ = 0U;
  uplink_write_index_ = 0U;
  uplink_count_ = 0U;
  user_turn_id_ = 0U;
  response_turn_id_ = 0U;
  utterance_frame_count_ = 0U;
  trailing_silence_frames_ = 0U;
  first_speech_deadline_us_ = 0U;
  capture_phase_ = CapturePhase::kMonitoring;
  playback_active_ = false;
  duck_requested_ = false;
  barge_in_speech_frames_ = 0U;
}

void VadUtteranceController::ResetPlaybackState() noexcept {
  playback_active_ = false;
  response_turn_id_ = 0U;
  barge_in_speech_frames_ = 0U;
  duck_requested_ = false;
}

void VadUtteranceController::PushPreRoll(
    const Mono16kFrame& frame) noexcept {
  pre_roll_[pre_roll_write_index_] = frame;
  pre_roll_write_index_ =
      (pre_roll_write_index_ + 1U) % kVadPreRollFrameCount;
  if (pre_roll_count_ < kVadPreRollFrameCount) {
    ++pre_roll_count_;
  }
}

bool VadUtteranceController::EnqueueFrame(const Mono16kFrame& frame,
                                          const std::uint32_t turn_id) noexcept {
  if (uplink_count_ >= kVadUplinkQueueCapacity || turn_id == 0U) {
    return false;
  }
  Mono16kFrame& destination = uplink_queue_[uplink_write_index_];
  destination = frame;
  destination.metadata.turn_id = turn_id;
  uplink_write_index_ =
      (uplink_write_index_ + 1U) % kVadUplinkQueueCapacity;
  ++uplink_count_;
  return true;
}

bool VadUtteranceController::EnqueuePreRoll(
    const std::uint32_t turn_id) noexcept {
  if (pre_roll_count_ != kVadPreRollFrameCount || turn_id == 0U ||
      kVadUplinkQueueCapacity - uplink_count_ < kVadPreRollFrameCount) {
    return false;
  }

  const std::size_t oldest_index = pre_roll_write_index_;
  for (std::size_t offset = 0U; offset < kVadPreRollFrameCount; ++offset) {
    const std::size_t source_index =
        (oldest_index + offset) % kVadPreRollFrameCount;
    if (!EnqueueFrame(pre_roll_[source_index], turn_id)) {
      return false;
    }
  }
  return true;
}

VadControllerResult VadUtteranceController::StartUtterance(
    const std::uint32_t initial_speech_frames,
    const bool barge_in) noexcept {
  if (pre_roll_count_ != kVadPreRollFrameCount) {
    return LatchFault(VadUtteranceEndReason::kPreRollUnavailable);
  }
  if (!EnqueuePreRoll(user_turn_id_)) {
    return LatchFault(VadUtteranceEndReason::kOutputOverflow);
  }

  VadControllerResult result = MakeResult(VadControllerCode::kUtteranceStarted);
  result.intents = AddIntent(result.intents,
                             VadControlIntent::kStartUtterance);
  if (barge_in) {
    result.intents = AddIntent(result.intents,
                               VadControlIntent::kInterruptConfirmed);
    result.intents = AddIntent(result.intents,
                               VadControlIntent::kCancelResponse);
    result.intents = AddIntent(result.intents,
                               VadControlIntent::kClearPlaybackQueue);
  }

  capture_phase_ = CapturePhase::kStreamingUtterance;
  utterance_frame_count_ = initial_speech_frames;
  trailing_silence_frames_ = 0U;
  first_speech_deadline_us_ = 0U;
  ResetPlaybackState();
  return result;
}

VadControllerResult VadUtteranceController::ProcessPlaybackObservation(
    const VadObservation observation) noexcept {
  if (observation == VadObservation::kSilence) {
    VadControllerResult result = MakeResult(VadControllerCode::kAccepted);
    if (duck_requested_) {
      result.intents = AddIntent(result.intents,
                                 VadControlIntent::kRestorePlayback);
    }
    barge_in_speech_frames_ = 0U;
    duck_requested_ = false;
    return result;
  }

  ++barge_in_speech_frames_;
  if (barge_in_speech_frames_ == kVadDuckCandidateFrameCount) {
    duck_requested_ = true;
    VadControllerResult result = MakeResult(VadControllerCode::kAccepted);
    result.intents = AddIntent(result.intents,
                               VadControlIntent::kDuckPlayback);
    return result;
  }
  if (barge_in_speech_frames_ >= kVadBargeInConfirmFrameCount) {
    return StartUtterance(barge_in_speech_frames_, true);
  }
  return MakeResult(VadControllerCode::kAccepted);
}

VadControlIntentMask
VadUtteranceController::ActiveCancellationIntents() const noexcept {
  VadControlIntentMask intents = ToMask(VadControlIntent::kNone);
  if (user_turn_id_ != 0U ||
      capture_phase_ != CapturePhase::kMonitoring) {
    intents = AddIntent(intents, VadControlIntent::kCancelUserTurn);
  }
  if (playback_active_) {
    if (duck_requested_) {
      intents = AddIntent(intents, VadControlIntent::kRestorePlayback);
    }
    intents = AddIntent(intents, VadControlIntent::kCancelResponse);
    intents = AddIntent(intents, VadControlIntent::kClearPlaybackQueue);
  }
  return intents;
}

}  // namespace boompi::audio
