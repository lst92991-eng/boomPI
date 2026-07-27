#include "boompi/audio/playback_frame.h"

#include <cstddef>

namespace boompi::audio {

bool TtsPcmFrame24k::HasValidLength() const noexcept {
  if (format.sample_rate_hz != kSampleRateHz ||
      format.frame_duration_ms != kFrameDurationMs ||
      format.channels != kChannelCount ||
      format.sample_format != kSampleFormat || metadata.epoch == 0U ||
      metadata.stream_id == 0U || metadata.turn_id == 0U ||
      valid_source_samples == 0U || valid_source_samples > kFrameSamples) {
    return false;
  }

  if (valid_source_samples == kFrameSamples) {
    return true;
  }
  if (!end_of_stream) {
    return false;
  }

  for (std::size_t sample_index = valid_source_samples;
       sample_index < samples.size(); ++sample_index) {
    if (samples[sample_index] != 0) {
      return false;
    }
  }
  return true;
}

void TtsPcmFrame24k::ResetHeader() noexcept {
  format = AudioFormat{};
  metadata = AudioFrameMetadata{};
  valid_source_samples = 0U;
  end_of_stream = false;
}

bool RenderReferenceFrame48k::HasValidLength() const noexcept {
  const bool timing_source_valid =
      timing_source == PlaybackTimingSource::kSoftwareEstimate ||
      timing_source == PlaybackTimingSource::kAlsaStatus;
  if (format.sample_rate_hz != kSampleRateHz ||
      format.frame_duration_ms != kFrameDurationMs ||
      (format.channels != 1U && format.channels != kMaximumChannels) ||
      format.sample_format != kSampleFormat ||
      samples_per_channel != kSamplesPerChannel || metadata.epoch == 0U ||
      metadata.stream_id == 0U ||
      metadata.monotonic_timestamp_us == 0U ||
      render_completed_timestamp_us == 0U ||
      metadata.monotonic_timestamp_us < render_completed_timestamp_us ||
      queued_sample_frames_before_write > kMaximumQueuedSampleFrames ||
      !timing_source_valid) {
    return false;
  }

  const auto interleaved_sample_count =
      static_cast<std::size_t>(samples_per_channel) *
      static_cast<std::size_t>(format.channels);
  return interleaved_sample_count <= samples.size();
}

std::size_t RenderReferenceFrame48k::interleaved_samples() const noexcept {
  if (!HasValidLength()) {
    return 0U;
  }
  return static_cast<std::size_t>(samples_per_channel) *
         static_cast<std::size_t>(format.channels);
}

void RenderReferenceFrame48k::ResetHeader() noexcept {
  format = AudioFormat{};
  metadata = AudioFrameMetadata{};
  samples_per_channel = 0U;
  render_completed_timestamp_us = 0U;
  queued_sample_frames_before_write = 0U;
  timing_source = PlaybackTimingSource::kUnset;
}

}  // namespace boompi::audio
