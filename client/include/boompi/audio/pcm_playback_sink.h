#ifndef BOOMPI_AUDIO_PCM_PLAYBACK_SINK_H_
#define BOOMPI_AUDIO_PCM_PLAYBACK_SINK_H_

#include <cstdint>

#include "boompi/audio/playback_frame.h"

namespace boompi::audio {

enum class PcmPlaybackWriteCode : std::uint8_t {
  kAccepted = 0,
  kWouldBlock,
  kInterrupted,
  kXrun,
  kSuspended,
  kDeviceLost,
  kZeroProgress,
  kFatal,
  kUnavailable,
};

struct PcmPlaybackWriteResult final {
  static constexpr std::uint32_t kMaximumQueuedSampleFrames = 72000U;

  PcmPlaybackWriteCode code;
  std::uint16_t accepted_sample_frames;
  std::uint64_t write_completed_timestamp_us;
  std::uint64_t estimated_presentation_timestamp_us;
  std::uint32_t queued_sample_frames_before_write;
  PlaybackTimingSource timing_source;
  // Optional backend-native status (for example a negative ALSA errno).
  // Core policy must branch only on `code`; zero means no native detail.
  std::int32_t native_error;

  constexpr bool accepted() const noexcept {
    return code == PcmPlaybackWriteCode::kAccepted && valid();
  }

  constexpr bool retryable_without_reset() const noexcept {
    return valid() &&
           (code == PcmPlaybackWriteCode::kWouldBlock ||
            code == PcmPlaybackWriteCode::kInterrupted);
  }

  constexpr bool requires_stream_reset() const noexcept {
    return valid() &&
           (code == PcmPlaybackWriteCode::kXrun ||
            code == PcmPlaybackWriteCode::kSuspended ||
            code == PcmPlaybackWriteCode::kDeviceLost ||
            code == PcmPlaybackWriteCode::kZeroProgress ||
            code == PcmPlaybackWriteCode::kFatal ||
            code == PcmPlaybackWriteCode::kUnavailable);
  }

  constexpr bool valid() const noexcept {
    switch (code) {
      case PcmPlaybackWriteCode::kAccepted:
        return accepted_sample_frames != 0U &&
               write_completed_timestamp_us != 0U &&
               estimated_presentation_timestamp_us >=
                   write_completed_timestamp_us &&
               queued_sample_frames_before_write <=
                   kMaximumQueuedSampleFrames &&
               HasKnownTimingSource() && native_error == 0;
      case PcmPlaybackWriteCode::kWouldBlock:
      case PcmPlaybackWriteCode::kInterrupted:
      case PcmPlaybackWriteCode::kXrun:
      case PcmPlaybackWriteCode::kSuspended:
      case PcmPlaybackWriteCode::kDeviceLost:
      case PcmPlaybackWriteCode::kZeroProgress:
      case PcmPlaybackWriteCode::kFatal:
      case PcmPlaybackWriteCode::kUnavailable:
        return HasEmptyWriteDetails();
    }
    return false;
  }

 private:
  constexpr bool HasKnownTimingSource() const noexcept {
    return timing_source == PlaybackTimingSource::kSoftwareEstimate ||
           timing_source == PlaybackTimingSource::kAlsaStatus;
  }

  constexpr bool HasEmptyWriteDetails() const noexcept {
    return accepted_sample_frames == 0U &&
           write_completed_timestamp_us == 0U &&
           estimated_presentation_timestamp_us == 0U &&
           queued_sample_frames_before_write == 0U &&
           timing_source == PlaybackTimingSource::kUnset;
  }
};

enum class PcmPlaybackControlCode : std::uint8_t {
  // Zero/value initialization must fail closed at this control boundary.
  kUnset = 0,
  kSucceeded,
  kInterrupted,
  kDeviceLost,
  kFatal,
  kUnavailable,
};

struct PcmPlaybackControlResult final {
  PcmPlaybackControlCode code;
  // Optional backend-native status. It is zero for successful controls.
  std::int32_t native_error;

  constexpr bool succeeded() const noexcept {
    return code == PcmPlaybackControlCode::kSucceeded && valid();
  }

  constexpr bool valid() const noexcept {
    switch (code) {
      case PcmPlaybackControlCode::kUnset:
        return false;
      case PcmPlaybackControlCode::kSucceeded:
        return native_error == 0;
      case PcmPlaybackControlCode::kInterrupted:
      case PcmPlaybackControlCode::kDeviceLost:
      case PcmPlaybackControlCode::kFatal:
      case PcmPlaybackControlCode::kUnavailable:
        return true;
    }
    return false;
  }
};

// Portable boundary for one preconfigured interleaved S16 playback stream.
// One playback worker exclusively owns an instance and serializes every
// method. `frames` is a frame count, not a scalar-sample or byte count. The
// sink never retains `samples` after TryWriteInterleavedS16 returns.
//
// A positive backend write is represented only by kAccepted and carries the
// exact accepted prefix plus a bounded presentation estimate. Every other
// code accepts zero frames and carries empty timing details. These estimates
// do not prove that PCM reached the DAC or became audible. Implementations
// must perform no allocation, locking, logging, or unrelated I/O per call.
class PcmPlaybackSink {
 public:
  PcmPlaybackSink() noexcept = default;
  PcmPlaybackSink(const PcmPlaybackSink&) = delete;
  PcmPlaybackSink& operator=(const PcmPlaybackSink&) = delete;
  PcmPlaybackSink(PcmPlaybackSink&&) = delete;
  PcmPlaybackSink& operator=(PcmPlaybackSink&&) = delete;
  virtual ~PcmPlaybackSink() noexcept = default;

  virtual PcmPlaybackWriteResult TryWriteInterleavedS16(
      const std::int16_t* samples,
      std::uint16_t frames) noexcept = 0;
  virtual PcmPlaybackControlResult Drop() noexcept = 0;
  virtual PcmPlaybackControlResult Prepare() noexcept = 0;
};

// Fail-closed default used when no verified platform playback adapter exists.
// It never accepts PCM and never claims that Drop or Prepare succeeded.
class UnavailablePcmPlaybackSink final : public PcmPlaybackSink {
 public:
  PcmPlaybackWriteResult TryWriteInterleavedS16(
      const std::int16_t* samples,
      std::uint16_t frames) noexcept override;
  PcmPlaybackControlResult Drop() noexcept override;
  PcmPlaybackControlResult Prepare() noexcept override;
};

}  // namespace boompi::audio

#endif  // BOOMPI_AUDIO_PCM_PLAYBACK_SINK_H_
