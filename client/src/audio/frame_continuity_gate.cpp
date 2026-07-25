#include "boompi/audio/frame_continuity_gate.h"

#include <limits>

namespace boompi::audio {

Status FrameContinuityGate::Arm(const std::uint32_t epoch,
                                const std::uint32_t stream_id) {
  if (epoch == 0U) {
    return Status::Error(StatusCode::kInvalidArgument,
                         "frame continuity epoch must be non-zero");
  }
  if (stream_id == 0U) {
    return Status::Error(StatusCode::kInvalidArgument,
                         "frame continuity stream id must be non-zero");
  }
  if (epoch_ != 0U && stream_id_ != 0U && epoch == epoch_ &&
      stream_id == stream_id_) {
    return Status::Error(
        StatusCode::kFailedPrecondition,
        "frame continuity re-arm requires a new epoch or stream id");
  }

  epoch_ = epoch;
  stream_id_ = stream_id;
  last_sequence_ = 0U;
  last_timestamp_us_ = 0U;
  armed_ = true;
  has_previous_frame_ = false;
  faulted_ = false;
  return Status::Ok();
}

void FrameContinuityGate::Disarm() noexcept {
  // Keep the retired identity so queued frames from that generation cannot be
  // made current again by a Disarm/Arm cycle.
  last_sequence_ = 0U;
  last_timestamp_us_ = 0U;
  armed_ = false;
  has_previous_frame_ = false;
  faulted_ = false;
}

FrameContinuityResult FrameContinuityGate::CheckAndAdvance(
    const AudioFrameMetadata& metadata) noexcept {
  if (!armed_) {
    return FrameContinuityResult::kNotArmed;
  }
  if (metadata.epoch != epoch_) {
    return FrameContinuityResult::kEpochMismatch;
  }
  if (metadata.stream_id != stream_id_) {
    return FrameContinuityResult::kStreamMismatch;
  }
  if (faulted_) {
    return FrameContinuityResult::kFaulted;
  }
  if (metadata.discontinuity) {
    LatchFault();
    return FrameContinuityResult::kDiscontinuity;
  }

  if (!has_previous_frame_) {
    last_sequence_ = metadata.sequence;
    last_timestamp_us_ = metadata.monotonic_timestamp_us;
    has_previous_frame_ = true;
    return FrameContinuityResult::kAcceptedFirst;
  }

  if (last_sequence_ == std::numeric_limits<std::uint32_t>::max()) {
    LatchFault();
    return FrameContinuityResult::kSequenceBreak;
  }
  const std::uint32_t expected_sequence = last_sequence_ + 1U;
  if (metadata.sequence != expected_sequence) {
    LatchFault();
    return FrameContinuityResult::kSequenceBreak;
  }
  if (metadata.monotonic_timestamp_us <= last_timestamp_us_) {
    LatchFault();
    return FrameContinuityResult::kTimestampNotIncreasing;
  }

  last_sequence_ = metadata.sequence;
  last_timestamp_us_ = metadata.monotonic_timestamp_us;
  return FrameContinuityResult::kAcceptedContinuous;
}

void FrameContinuityGate::LatchFault() noexcept { faulted_ = true; }

}  // namespace boompi::audio
