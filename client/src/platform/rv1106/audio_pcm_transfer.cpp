#include "boompi/platform/audio_pcm_transfer.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace boompi::platform {
namespace {

constexpr std::uint32_t kCaptureSampleFrames = 960U;
constexpr std::uint64_t kCapturePeriodUs = 20000U;
constexpr std::uint32_t kMaximumWaitSliceMs = 100U;
constexpr std::uint32_t kMaximumOperationTimeoutMs = 5000U;

bool ValidTransferConfig(const AudioPeriodTransferConfig& config) noexcept {
  return config.wait_slice_ms > 0U &&
         config.wait_slice_ms <= kMaximumWaitSliceMs &&
         config.operation_timeout_ms >= config.wait_slice_ms &&
         config.operation_timeout_ms <= kMaximumOperationTimeoutMs;
}

std::uint64_t AddTimeoutUs(const std::uint64_t now_us,
                           const std::uint32_t timeout_ms) noexcept {
  const std::uint64_t timeout_us =
      static_cast<std::uint64_t>(timeout_ms) * 1000U;
  if (now_us > std::numeric_limits<std::uint64_t>::max() - timeout_us) {
    return std::numeric_limits<std::uint64_t>::max();
  }
  return now_us + timeout_us;
}

std::uint32_t RemainingWaitMs(const std::uint64_t now_us,
                              const std::uint64_t deadline_us,
                              const std::uint32_t wait_slice_ms) noexcept {
  if (now_us >= deadline_us) {
    return 0U;
  }
  const std::uint64_t remaining_us = deadline_us - now_us;
  const std::uint64_t rounded_remaining_ms = (remaining_us + 999U) / 1000U;
  return static_cast<std::uint32_t>(
      std::min<std::uint64_t>(wait_slice_ms, rounded_remaining_ms));
}

audio::PcmPlaybackWriteCode MapPlaybackWriteCode(
    const PcmBackendCode code) noexcept {
  switch (code) {
    case PcmBackendCode::kWouldBlock:
      return audio::PcmPlaybackWriteCode::kWouldBlock;
    case PcmBackendCode::kInterrupted:
      return audio::PcmPlaybackWriteCode::kInterrupted;
    case PcmBackendCode::kXrun:
      return audio::PcmPlaybackWriteCode::kXrun;
    case PcmBackendCode::kSuspended:
      return audio::PcmPlaybackWriteCode::kSuspended;
    case PcmBackendCode::kTimeout:
      return audio::PcmPlaybackWriteCode::kZeroProgress;
    case PcmBackendCode::kReady:
    case PcmBackendCode::kFrames:
    case PcmBackendCode::kFatal:
      return audio::PcmPlaybackWriteCode::kFatal;
  }
  return audio::PcmPlaybackWriteCode::kFatal;
}

audio::PcmPlaybackWriteResult EmptyPlaybackWriteResult(
    const audio::PcmPlaybackWriteCode code) noexcept {
  return {code, 0U, 0U, 0U, 0U, audio::PlaybackTimingSource::kUnset, 0};
}

audio::PcmPlaybackControlResult PlaybackControlResult(
    const bool available, const bool succeeded) noexcept {
  if (!available) {
    return {audio::PcmPlaybackControlCode::kUnavailable, 0};
  }
  return {succeeded ? audio::PcmPlaybackControlCode::kSucceeded
                    : audio::PcmPlaybackControlCode::kFatal,
          0};
}

}  // namespace

std::uint64_t SteadyAudioIoClock::NowUs() const noexcept {
  const auto now = std::chrono::steady_clock::now().time_since_epoch();
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(now).count());
}

void AudioIoStopFlag::RequestStop() noexcept {
  requested_.store(1U, std::memory_order_release);
}

bool AudioIoStopFlag::stop_requested() const noexcept {
  return requested_.load(std::memory_order_acquire) != 0U;
}

Status CaptureEpochFence::AdvanceTo(const std::uint32_t epoch) {
  if (epoch == 0U) {
    return Status::Error(StatusCode::kInvalidArgument,
                         "capture fence epoch must be non-zero");
  }
  const std::uint32_t current = epoch_.load(std::memory_order_relaxed);
  if (current == std::numeric_limits<std::uint32_t>::max()) {
    return Status::Error(
        StatusCode::kResourceExhausted,
        "capture fence epoch is exhausted; quiescent restart is required");
  }
  if (epoch <= current) {
    return Status::Error(StatusCode::kFailedPrecondition,
                         "capture fence epoch must increase without reuse");
  }
  epoch_.store(epoch, std::memory_order_release);
  return Status::Ok();
}

std::uint32_t CaptureEpochFence::Observe() const noexcept {
  return epoch_.load(std::memory_order_acquire);
}

PcmPlaybackSink48k::PcmPlaybackSink48k(
    PcmStreamBackend* const backend, const AudioIoClock* const clock) noexcept
    : backend_(backend), clock_(clock) {}

audio::PcmPlaybackWriteResult PcmPlaybackSink48k::TryWriteInterleavedS16(
    const std::int16_t* const samples, const std::uint16_t frames) noexcept {
  if (backend_ == nullptr || clock_ == nullptr || samples == nullptr ||
      frames == 0U ||
      frames > audio::AcceptedRenderChunk48k::kSampleCapacity) {
    return EmptyPlaybackWriteResult(audio::PcmPlaybackWriteCode::kFatal);
  }

  std::uint32_t queued_sample_frames = 0U;
  if (!backend_->QueryQueuedSampleFrames(&queued_sample_frames) ||
      queued_sample_frames >
          audio::PcmPlaybackWriteResult::kMaximumQueuedSampleFrames) {
    return EmptyPlaybackWriteResult(audio::PcmPlaybackWriteCode::kFatal);
  }

  for (std::uint16_t sample_frame = 0U; sample_frame < frames;
       ++sample_frame) {
    const std::int16_t sample = samples[sample_frame];
    const std::size_t stereo_offset =
        static_cast<std::size_t>(sample_frame) * 2U;
    stereo_scratch_[stereo_offset] = sample;
    stereo_scratch_[stereo_offset + 1U] = sample;
  }

  const PcmBackendResult transfer =
      backend_->WriteInterleaved(stereo_scratch_.data(), frames);
  const std::uint16_t accepted = static_cast<std::uint16_t>(
      std::min<std::uint32_t>(transfer.sample_frames,
                              std::numeric_limits<std::uint16_t>::max()));
  if (transfer.sample_frames == 0U) {
    if (transfer.code == PcmBackendCode::kFrames) {
      return EmptyPlaybackWriteResult(
          audio::PcmPlaybackWriteCode::kZeroProgress);
    }
    return EmptyPlaybackWriteResult(MapPlaybackWriteCode(transfer.code));
  }

  if (transfer.code != PcmBackendCode::kFrames) {
    audio::PcmPlaybackWriteResult malformed =
        EmptyPlaybackWriteResult(MapPlaybackWriteCode(transfer.code));
    malformed.accepted_sample_frames = accepted;
    return malformed;
  }

  const std::uint64_t write_completed_us = clock_->NowUs();
  const std::uint64_t queue_delay_us =
      static_cast<std::uint64_t>(queued_sample_frames) * 1000000U /
      audio::AcceptedRenderChunk48k::kSampleRateHz;
  if (write_completed_us == 0U ||
      write_completed_us >
          std::numeric_limits<std::uint64_t>::max() - queue_delay_us) {
    // The device may already have accepted this positive prefix. Preserve the
    // count in an intentionally malformed result so PlaybackCommitter advances
    // exactly once, publishes no untrusted timing, and requires cancellation.
    return {audio::PcmPlaybackWriteCode::kAccepted, accepted, 0U, 0U, 0U,
            audio::PlaybackTimingSource::kUnset, 0};
  }

  return {audio::PcmPlaybackWriteCode::kAccepted,
          accepted,
          write_completed_us,
          write_completed_us + queue_delay_us,
          queued_sample_frames,
          audio::PlaybackTimingSource::kSoftwareEstimate,
          0};
}

audio::PcmPlaybackControlResult PcmPlaybackSink48k::Drop() noexcept {
  return PlaybackControlResult(backend_ != nullptr,
                               backend_ != nullptr && backend_->Drop());
}

audio::PcmPlaybackControlResult PcmPlaybackSink48k::Prepare() noexcept {
  return PlaybackControlResult(backend_ != nullptr,
                               backend_ != nullptr && backend_->Prepare());
}

CapturePeriodReader48k::CapturePeriodReader48k(
    PcmStreamBackend* const backend, const AudioIoClock* const clock) noexcept
    : backend_(backend), clock_(clock) {}

Status CapturePeriodReader48k::Configure(
    const AudioPeriodTransferConfig& config,
    const std::uint8_t capture_channels) {
  if (configured_) {
    return Status::Error(StatusCode::kFailedPrecondition,
                         "capture PCM reader is already configured");
  }
  if (backend_ == nullptr || clock_ == nullptr) {
    return Status::Error(StatusCode::kInvalidArgument,
                         "capture PCM reader dependencies must not be null");
  }
  if (!ValidTransferConfig(config) ||
      (capture_channels != 2U && capture_channels != 4U)) {
    return Status::Error(StatusCode::kInvalidArgument,
                         "capture PCM channels/wait/timeout are invalid");
  }
  config_ = config;
  capture_channels_ = capture_channels;
  configured_ = true;
  return Status::Ok();
}

Status CapturePeriodReader48k::Activate(
    const std::uint32_t epoch, const std::uint32_t stream_id,
    const CaptureEpochFence& epoch_fence) {
  if (!configured_) {
    return Status::Error(StatusCode::kFailedPrecondition,
                         "capture PCM reader is not configured");
  }
  if (epoch == 0U || stream_id == 0U || epoch_fence.Observe() != epoch) {
    return Status::Error(StatusCode::kInvalidArgument,
                         "capture generation must match a non-zero fence");
  }
  if (active_) {
    return Status::Error(StatusCode::kFailedPrecondition,
                         "capture generation is already active");
  }
  if (epoch == retired_epoch_ && stream_id == retired_stream_id_) {
    return Status::Error(StatusCode::kFailedPrecondition,
                         "retired capture generation cannot be reused");
  }
  if (!sequencer_.Reset(epoch, stream_id)) {
    return Status::Error(StatusCode::kFailedPrecondition,
                         "capture sequencer rejected generation");
  }
  active_epoch_ = epoch;
  active_stream_id_ = stream_id;
  active_ = true;
  return Status::Ok();
}

AudioIoCancelResult CapturePeriodReader48k::Cancel() noexcept {
  if (!active_) {
    return {AudioIoCancelCode::kNotActive};
  }
  const bool reset = backend_->Reset();
  retired_epoch_ = active_epoch_;
  retired_stream_id_ = active_stream_id_;
  active_epoch_ = 0U;
  active_stream_id_ = 0U;
  active_ = false;
  return {reset ? AudioIoCancelCode::kAcknowledged
                : AudioIoCancelCode::kBackendFault};
}

CaptureTransferResult CapturePeriodReader48k::Read(
    const std::uint32_t turn_id, const CaptureEpochFence& epoch_fence,
    const AudioIoStopFlag& stop_flag,
    audio::CaptureFrame* const output) noexcept {
  if (output == nullptr) {
    return {CaptureTransferCode::kInvalidArgument, 0U};
  }
  output->ResetHeader();
  if (!configured_ || !active_) {
    return {CaptureTransferCode::kNotActive, 0U};
  }
  if (epoch_fence.Observe() != active_epoch_) {
    const AudioIoCancelResult cancelled = Cancel();
    return {cancelled.acknowledged()
                ? CaptureTransferCode::kGenerationMismatch
                : CaptureTransferCode::kBackendFault,
            0U};
  }
  if (stop_flag.stop_requested()) {
    const AudioIoCancelResult cancelled = Cancel();
    return {cancelled.acknowledged() ? CaptureTransferCode::kCancelled
                                    : CaptureTransferCode::kBackendFault,
            0U};
  }

  const std::uint64_t deadline_us =
      AddTimeoutUs(clock_->NowUs(), config_.operation_timeout_ms);
  for (;;) {
    if (stop_flag.stop_requested() ||
        epoch_fence.Observe() != active_epoch_) {
      const AudioIoCancelResult cancelled = Cancel();
      return {cancelled.acknowledged() ? CaptureTransferCode::kCancelled
                                      : CaptureTransferCode::kBackendFault,
              0U};
    }
    if (clock_->NowUs() >= deadline_us) {
      MarkDiscontinuity();
      return {CaptureTransferCode::kTimeout, 0U};
    }

    const PcmBackendResult transfer = backend_->ReadInterleaved(
        capture_scratch_.data(), kCaptureSampleFrames);
    if (transfer.code == PcmBackendCode::kFrames) {
      if (transfer.sample_frames != kCaptureSampleFrames) {
        const std::uint32_t discarded =
            std::min(transfer.sample_frames, kCaptureSampleFrames);
        AccountDiscardedPeriod();
        if (transfer.sample_frames > kCaptureSampleFrames) {
          static_cast<void>(Cancel());
          return {CaptureTransferCode::kBackendFault, discarded};
        }
        return {CaptureTransferCode::kPartialDiscarded, discarded};
      }

      output->format = {48000U, 20U, capture_channels_,
                        audio::SampleFormat::kPcmS16Le};
      const std::uint64_t completed_us = clock_->NowUs();
      const std::uint64_t first_sample_us =
          completed_us > kCapturePeriodUs ? completed_us - kCapturePeriodUs
                                          : 1U;
      output->metadata = sequencer_.NextPublished(first_sample_us, turn_id);
      output->samples_per_channel = kCaptureSampleFrames;
      const std::size_t interleaved_samples =
          static_cast<std::size_t>(kCaptureSampleFrames) * capture_channels_;
      std::copy_n(capture_scratch_.begin(), interleaved_samples,
                  output->samples.begin());
      if (!output->HasValidLength()) {
        output->ResetHeader();
        static_cast<void>(Cancel());
        return {CaptureTransferCode::kBackendFault, 0U};
      }
      return {CaptureTransferCode::kCaptured, 0U};
    }

    if (transfer.code == PcmBackendCode::kWouldBlock ||
        transfer.code == PcmBackendCode::kInterrupted) {
      const std::uint32_t wait_ms =
          RemainingWaitMs(clock_->NowUs(), deadline_us,
                          config_.wait_slice_ms);
      if (wait_ms == 0U) {
        MarkDiscontinuity();
        return {CaptureTransferCode::kTimeout, 0U};
      }
      const PcmBackendResult wait_result = backend_->Wait(wait_ms);
      if (wait_result.code == PcmBackendCode::kReady ||
          wait_result.code == PcmBackendCode::kTimeout ||
          wait_result.code == PcmBackendCode::kInterrupted) {
        continue;
      }
      if (wait_result.code == PcmBackendCode::kXrun ||
          wait_result.code == PcmBackendCode::kSuspended) {
        const bool recovered = backend_->Recover(wait_result.code);
        MarkDiscontinuity();
        if (!recovered) {
          static_cast<void>(Cancel());
          return {CaptureTransferCode::kBackendFault, 0U};
        }
        return {wait_result.code == PcmBackendCode::kXrun
                    ? CaptureTransferCode::kXrun
                    : CaptureTransferCode::kSuspended,
                0U};
      }
      static_cast<void>(Cancel());
      return {CaptureTransferCode::kBackendFault, 0U};
    }

    if (transfer.code == PcmBackendCode::kXrun ||
        transfer.code == PcmBackendCode::kSuspended) {
      const bool recovered = backend_->Recover(transfer.code);
      MarkDiscontinuity();
      if (!recovered) {
        static_cast<void>(Cancel());
        return {CaptureTransferCode::kBackendFault, 0U};
      }
      return {transfer.code == PcmBackendCode::kXrun
                  ? CaptureTransferCode::kXrun
                  : CaptureTransferCode::kSuspended,
              0U};
    }
    static_cast<void>(Cancel());
    return {CaptureTransferCode::kBackendFault, 0U};
  }
}

void CapturePeriodReader48k::AccountDiscardedPeriod() noexcept {
  sequencer_.AccountDroppedFrame();
}

void CapturePeriodReader48k::MarkDiscontinuity() noexcept {
  sequencer_.MarkDiscontinuity();
}

}  // namespace boompi::platform
