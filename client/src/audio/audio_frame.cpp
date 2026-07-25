#include "boompi/audio/audio_frame.h"

#include <limits>

namespace boompi::audio {

bool AudioFrameSequencer::Reset(const std::uint32_t epoch,
                                const std::uint32_t stream_id,
                                const std::uint32_t next_sequence) noexcept {
  if (epoch == 0U || stream_id == 0U) {
    return false;
  }
  if (initialized() && epoch == epoch_ && stream_id == stream_id_) {
    return false;
  }

  epoch_ = epoch;
  stream_id_ = stream_id;
  next_sequence_ = next_sequence;
  discontinuity_pending_ = false;
  return true;
}

AudioFrameMetadata AudioFrameSequencer::NextPublished(
    const std::uint64_t monotonic_timestamp_us,
    const std::uint32_t turn_id) noexcept {
  if (!initialized()) {
    return AudioFrameMetadata{};
  }

  AudioFrameMetadata metadata{};
  metadata.monotonic_timestamp_us = monotonic_timestamp_us;
  metadata.sequence = next_sequence_;
  metadata.stream_id = stream_id_;
  metadata.turn_id = turn_id;
  metadata.epoch = epoch_;
  metadata.discontinuity = discontinuity_pending_;
  discontinuity_pending_ = false;
  if (next_sequence_ == std::numeric_limits<std::uint32_t>::max()) {
    next_sequence_ = 0U;
    discontinuity_pending_ = true;
  } else {
    ++next_sequence_;
  }
  return metadata;
}

void AudioFrameSequencer::AccountDroppedFrame() noexcept {
  if (!initialized()) {
    return;
  }
  ++next_sequence_;
  discontinuity_pending_ = true;
}

void AudioFrameSequencer::MarkDiscontinuity() noexcept {
  if (initialized()) {
    discontinuity_pending_ = true;
  }
}

bool AudioFrameSequencer::initialized() const noexcept {
  return epoch_ != 0U && stream_id_ != 0U;
}

}  // namespace boompi::audio
