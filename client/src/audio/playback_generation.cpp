#include "boompi/audio/playback_generation.h"

#include <limits>

namespace boompi::audio {
namespace {

bool GenerationsEqual(const PlaybackGeneration& lhs,
                      const PlaybackGeneration& rhs) noexcept {
  return lhs.epoch == rhs.epoch && lhs.turn_id == rhs.turn_id &&
         lhs.stream_id == rhs.stream_id;
}

}  // namespace

bool PlaybackGeneration::valid() const noexcept {
  return epoch != 0U && turn_id != 0U && stream_id != 0U;
}

Status PlaybackGenerationGate::Activate(
    const PlaybackGeneration& generation,
    const std::uint32_t observed_fence_epoch) {
  if (!generation.valid()) {
    return Status::Error(
        StatusCode::kInvalidArgument,
        "playback generation epoch, turn id, and stream id must be non-zero");
  }
  if (observed_fence_epoch == 0U) {
    return Status::Error(
        StatusCode::kFailedPrecondition,
        "playback generation requires a non-zero observed fence epoch");
  }
  if (observed_fence_epoch != generation.epoch) {
    return Status::Error(
        StatusCode::kFailedPrecondition,
        "playback generation epoch does not match the observed fence epoch");
  }
  if (active_) {
    return Status::Error(
        StatusCode::kFailedPrecondition,
        "the active playback generation must be retired before activation");
  }
  if (generation.epoch == retired_epoch_ &&
      generation.stream_id == retired_stream_id_) {
    return Status::Error(
        StatusCode::kFailedPrecondition,
        "the most recently retired playback epoch and stream cannot be reused");
  }

  active_generation_ = generation;
  active_ = true;
  return Status::Ok();
}

PlaybackGenerationRetireResult PlaybackGenerationGate::Retire(
    const PlaybackGeneration& expected_generation) noexcept {
  if (!active_) {
    return PlaybackGenerationRetireResult::kNotActive;
  }
  if (!GenerationsEqual(active_generation_, expected_generation)) {
    return PlaybackGenerationRetireResult::kExpectedGenerationMismatch;
  }

  RememberRetiredGeneration();
  return PlaybackGenerationRetireResult::kRetired;
}

PlaybackGenerationRetireResult
PlaybackGenerationGate::InvalidateIfEpochChanged(
    const std::uint32_t observed_epoch) noexcept {
  if (!active_) {
    return PlaybackGenerationRetireResult::kNotActive;
  }
  if (observed_epoch == active_generation_.epoch) {
    return PlaybackGenerationRetireResult::kEpochUnchanged;
  }

  RememberRetiredGeneration();
  return PlaybackGenerationRetireResult::kRetired;
}

PlaybackGenerationDecision PlaybackGenerationGate::Check(
    const PlaybackGeneration& generation) const noexcept {
  if (!generation.valid()) {
    return PlaybackGenerationDecision::kInvalidGeneration;
  }
  if (!active_) {
    return PlaybackGenerationDecision::kNotActive;
  }
  if (generation.epoch != active_generation_.epoch) {
    return PlaybackGenerationDecision::kEpochMismatch;
  }
  if (generation.stream_id != active_generation_.stream_id) {
    return PlaybackGenerationDecision::kStreamMismatch;
  }
  if (generation.turn_id != active_generation_.turn_id) {
    return PlaybackGenerationDecision::kTurnMismatch;
  }
  return PlaybackGenerationDecision::kAccepted;
}

void PlaybackGenerationGate::RememberRetiredGeneration() noexcept {
  retired_epoch_ = active_generation_.epoch;
  retired_stream_id_ = active_generation_.stream_id;
  active_generation_ = PlaybackGeneration{};
  active_ = false;
}

Status PlaybackEpochFence::AdvanceTo(const std::uint32_t epoch) {
  if (epoch == 0U) {
    return Status::Error(StatusCode::kInvalidArgument,
                         "playback fence epoch must be non-zero");
  }

  const std::uint32_t current = epoch_.load(std::memory_order_relaxed);
  if (current == std::numeric_limits<std::uint32_t>::max()) {
    return Status::Error(
        StatusCode::kResourceExhausted,
        "playback fence epoch is exhausted; quiescent restart is required");
  }
  if (epoch <= current) {
    return Status::Error(
        StatusCode::kFailedPrecondition,
        "playback fence epoch must increase without reuse or wraparound");
  }

  epoch_.store(epoch, std::memory_order_release);
  return Status::Ok();
}

std::uint32_t PlaybackEpochFence::Observe() const noexcept {
  return epoch_.load(std::memory_order_acquire);
}

bool PlaybackEpochFence::is_lock_free() const noexcept {
  return epoch_.is_lock_free();
}

}  // namespace boompi::audio
