#include "boompi/platform/audio_pcm_playback.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace boompi::platform {
namespace {

constexpr std::uint64_t kMicrosecondsPerSecond = 1000000U;

audio::PcmPlaybackWriteCode
MapWriteCode(const PcmPlaybackDeviceCode code) noexcept {
  switch (code) {
  case PcmPlaybackDeviceCode::kSucceeded:
    return audio::PcmPlaybackWriteCode::kAccepted;
  case PcmPlaybackDeviceCode::kWouldBlock:
    return audio::PcmPlaybackWriteCode::kWouldBlock;
  case PcmPlaybackDeviceCode::kInterrupted:
    return audio::PcmPlaybackWriteCode::kInterrupted;
  case PcmPlaybackDeviceCode::kXrun:
    return audio::PcmPlaybackWriteCode::kXrun;
  case PcmPlaybackDeviceCode::kSuspended:
    return audio::PcmPlaybackWriteCode::kSuspended;
  case PcmPlaybackDeviceCode::kDeviceLost:
    return audio::PcmPlaybackWriteCode::kDeviceLost;
  case PcmPlaybackDeviceCode::kZeroProgress:
    return audio::PcmPlaybackWriteCode::kZeroProgress;
  case PcmPlaybackDeviceCode::kUnavailable:
    return audio::PcmPlaybackWriteCode::kUnavailable;
  case PcmPlaybackDeviceCode::kUnset:
  case PcmPlaybackDeviceCode::kFatal:
    return audio::PcmPlaybackWriteCode::kFatal;
  }
  return audio::PcmPlaybackWriteCode::kFatal;
}

audio::PcmPlaybackWriteResult
EmptyWriteResult(const audio::PcmPlaybackWriteCode code,
                 const std::int32_t native_error = 0) noexcept {
  return {code,        0U, 0U, 0U, 0U, audio::PlaybackTimingSource::kUnset,
          native_error};
}

audio::PcmPlaybackWriteResult
MalformedPositiveWriteResult(const audio::PcmPlaybackWriteCode code,
                             const std::uint32_t accepted_sample_frames,
                             const std::int32_t native_error) noexcept {
  const auto bounded_accepted =
      static_cast<std::uint16_t>(std::min<std::uint32_t>(
          accepted_sample_frames, std::numeric_limits<std::uint16_t>::max()));
  return {code,        bounded_accepted,
          0U,          0U,
          0U,          audio::PlaybackTimingSource::kUnset,
          native_error};
}

audio::PcmPlaybackControlResult
MapControlResult(const PcmPlaybackDeviceControlResult &result) noexcept {
  switch (result.code) {
  case PcmPlaybackDeviceCode::kSucceeded:
    if (result.native_error == 0) {
      return {audio::PcmPlaybackControlCode::kSucceeded, 0};
    }
    return {audio::PcmPlaybackControlCode::kFatal, result.native_error};
  case PcmPlaybackDeviceCode::kInterrupted:
    return {audio::PcmPlaybackControlCode::kInterrupted, result.native_error};
  case PcmPlaybackDeviceCode::kDeviceLost:
    return {audio::PcmPlaybackControlCode::kDeviceLost, result.native_error};
  case PcmPlaybackDeviceCode::kUnavailable:
    return {audio::PcmPlaybackControlCode::kUnavailable, result.native_error};
  case PcmPlaybackDeviceCode::kUnset:
  case PcmPlaybackDeviceCode::kWouldBlock:
  case PcmPlaybackDeviceCode::kXrun:
  case PcmPlaybackDeviceCode::kSuspended:
  case PcmPlaybackDeviceCode::kZeroProgress:
  case PcmPlaybackDeviceCode::kFatal:
    return {audio::PcmPlaybackControlCode::kFatal, result.native_error};
  }
  return {audio::PcmPlaybackControlCode::kFatal, result.native_error};
}

} // namespace

Status PcmPlaybackSink48k::Create(PcmPlaybackDevice *const device,
                                  PcmPlaybackSink48k *const output) {
  if (output == nullptr) {
    return Status::Error(StatusCode::kInvalidArgument,
                         "playback sink output must not be null");
  }
  if (output->configured()) {
    return Status::Error(StatusCode::kFailedPrecondition,
                         "playback sink is already configured");
  }
  if (device == nullptr) {
    return Status::Error(StatusCode::kInvalidArgument,
                         "playback sink device must not be null");
  }

  const PcmPlaybackDeviceConfig device_config = device->config();
  if (!device_config.valid() ||
      device_config.sample_rate_hz !=
          audio::AcceptedRenderChunk48k::kSampleRateHz) {
    return Status::Error(
        StatusCode::kInvalidArgument,
        "playback sink requires an explicit valid 48 kHz device contract");
  }

  output->device_ = device;
  output->device_channels_ = device_config.channels;
  return Status::Ok();
}

audio::PcmPlaybackWriteResult PcmPlaybackSink48k::TryWriteInterleavedS16(
    const std::int16_t *const samples, const std::uint16_t frames) noexcept {
  if (!configured()) {
    return EmptyWriteResult(audio::PcmPlaybackWriteCode::kUnavailable);
  }
  if (samples == nullptr || frames == 0U ||
      frames > audio::AcceptedRenderChunk48k::kSampleCapacity ||
      (device_channels_ != 1U && device_channels_ != 2U)) {
    return EmptyWriteResult(audio::PcmPlaybackWriteCode::kFatal);
  }

  const PcmPlaybackDeviceStatusResult status = device_->QueryStatus();
  if (status.code != PcmPlaybackDeviceCode::kSucceeded) {
    return EmptyWriteResult(MapWriteCode(status.code), status.native_error);
  }
  if (status.native_error != 0 || status.monotonic_timestamp_us == 0U ||
      status.queued_sample_frames >
          audio::PcmPlaybackWriteResult::kMaximumQueuedSampleFrames ||
      (status.timeline_state != PcmPlaybackTimelineState::kPrepared &&
       status.timeline_state != PcmPlaybackTimelineState::kRunning)) {
    return EmptyWriteResult(audio::PcmPlaybackWriteCode::kFatal,
                            status.native_error);
  }

  const std::int16_t *interleaved_samples = samples;
  if (device_channels_ == 2U) {
    for (std::uint16_t frame = 0U; frame < frames; ++frame) {
      const std::size_t offset = static_cast<std::size_t>(frame) * 2U;
      interleaved_scratch_[offset] = samples[frame];
      interleaved_scratch_[offset + 1U] = samples[frame];
    }
    interleaved_samples = interleaved_scratch_.data();
  }

  const PcmPlaybackDeviceWriteResult write =
      device_->TryWriteInterleavedS16(interleaved_samples, frames);
  if (write.accepted_sample_frames == 0U) {
    if (write.code == PcmPlaybackDeviceCode::kSucceeded) {
      return EmptyWriteResult(audio::PcmPlaybackWriteCode::kZeroProgress,
                              write.native_error);
    }
    return EmptyWriteResult(MapWriteCode(write.code), write.native_error);
  }

  if (write.code != PcmPlaybackDeviceCode::kSucceeded ||
      write.native_error != 0 || write.accepted_sample_frames > frames) {
    const audio::PcmPlaybackWriteCode mapped_code =
        write.code == PcmPlaybackDeviceCode::kSucceeded
            ? audio::PcmPlaybackWriteCode::kFatal
            : MapWriteCode(write.code);
    return MalformedPositiveWriteResult(
        mapped_code, write.accepted_sample_frames, write.native_error);
  }

  const std::uint64_t write_completed_us = device_->MonotonicNowUs();
  if (write_completed_us == 0U) {
    return MalformedPositiveWriteResult(audio::PcmPlaybackWriteCode::kFatal,
                                        write.accepted_sample_frames, 0);
  }

  const PcmPlaybackDeviceStatusResult post_status = device_->QueryStatus();
  if (post_status.code != PcmPlaybackDeviceCode::kSucceeded) {
    return MalformedPositiveWriteResult(MapWriteCode(post_status.code),
                                        write.accepted_sample_frames,
                                        post_status.native_error);
  }
  const std::uint64_t observed_after_status_us = device_->MonotonicNowUs();
  if (post_status.native_error != 0 ||
      post_status.timeline_state != PcmPlaybackTimelineState::kRunning ||
      post_status.monotonic_timestamp_us == 0U ||
      post_status.queued_sample_frames >
          audio::PcmPlaybackWriteResult::kMaximumQueuedSampleFrames ||
      observed_after_status_us == 0U ||
      observed_after_status_us < write_completed_us ||
      post_status.monotonic_timestamp_us < write_completed_us ||
      post_status.monotonic_timestamp_us > observed_after_status_us) {
    return MalformedPositiveWriteResult(audio::PcmPlaybackWriteCode::kFatal,
                                        write.accepted_sample_frames,
                                        post_status.native_error);
  }

  const std::uint32_t queued_before_prefix_sample_frames =
      post_status.queued_sample_frames > write.accepted_sample_frames
          ? post_status.queued_sample_frames - write.accepted_sample_frames
          : 0U;
  const std::uint64_t presentation_delay_us =
      static_cast<std::uint64_t>(queued_before_prefix_sample_frames) *
      kMicrosecondsPerSecond / audio::AcceptedRenderChunk48k::kSampleRateHz;
  if (post_status.monotonic_timestamp_us >
      std::numeric_limits<std::uint64_t>::max() - presentation_delay_us) {
    return MalformedPositiveWriteResult(audio::PcmPlaybackWriteCode::kFatal,
                                        write.accepted_sample_frames, 0);
  }
  const std::uint64_t estimated_presentation_us =
      std::max(write_completed_us,
               post_status.monotonic_timestamp_us + presentation_delay_us);

  return {audio::PcmPlaybackWriteCode::kAccepted,
          static_cast<std::uint16_t>(write.accepted_sample_frames),
          write_completed_us,
          estimated_presentation_us,
          status.queued_sample_frames,
          audio::PlaybackTimingSource::kAlsaStatus,
          0};
}

audio::PcmPlaybackControlResult PcmPlaybackSink48k::Drop() noexcept {
  if (!configured()) {
    return {audio::PcmPlaybackControlCode::kUnavailable, 0};
  }
  return MapControlResult(device_->Drop());
}

audio::PcmPlaybackControlResult PcmPlaybackSink48k::Prepare() noexcept {
  if (!configured()) {
    return {audio::PcmPlaybackControlCode::kUnavailable, 0};
  }
  return MapControlResult(device_->Prepare());
}

} // namespace boompi::platform
