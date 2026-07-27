#ifndef BOOMPI_PLATFORM_AUDIO_PCM_TRANSFER_H_
#define BOOMPI_PLATFORM_AUDIO_PCM_TRANSFER_H_

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

#include "boompi/audio/audio_frame.h"
#include "boompi/audio/pcm_playback_sink.h"
#include "boompi/event/status.h"

namespace boompi::platform {

enum class PcmBackendCode : std::uint8_t {
  kReady = 0,
  kFrames,
  kWouldBlock,
  kTimeout,
  kInterrupted,
  kXrun,
  kSuspended,
  kFatal,
};

struct PcmBackendResult final {
  PcmBackendCode code{PcmBackendCode::kFatal};
  std::uint32_t sample_frames{0U};
};

// A concrete backend owns one PCM handle and is called by one audio worker.
// Wait and Transfer must be bounded/nonblocking; Recover and Reset are cold
// error/cancellation paths. Native ALSA error values never escape this boundary.
class PcmStreamBackend {
 public:
  virtual ~PcmStreamBackend() = default;
  virtual PcmBackendResult Wait(std::uint32_t timeout_ms) noexcept = 0;
  virtual PcmBackendResult ReadInterleaved(
      std::int16_t* samples, std::uint32_t requested_sample_frames) noexcept = 0;
  virtual PcmBackendResult WriteInterleaved(
      const std::int16_t* samples,
      std::uint32_t requested_sample_frames) noexcept = 0;
  virtual bool Recover(PcmBackendCode reason) noexcept = 0;
  virtual bool Drop() noexcept = 0;
  virtual bool Prepare() noexcept = 0;
  virtual bool Reset() noexcept = 0;
  virtual bool QueryQueuedSampleFrames(
      std::uint32_t* queued_sample_frames) noexcept = 0;
};

class AudioIoClock {
 public:
  virtual ~AudioIoClock() = default;
  virtual std::uint64_t NowUs() const noexcept = 0;
};

class SteadyAudioIoClock final : public AudioIoClock {
 public:
  std::uint64_t NowUs() const noexcept override;
};

// One control owner requests stop; the capture/playback worker only observes.
// This flag is a wake-up check between bounded PCM waits, not an acknowledgement.
class AudioIoStopFlag final {
  static_assert(std::atomic<std::uint32_t>::is_always_lock_free,
                "audio stop flag requires lock-free 32-bit atomics");

 public:
  void RequestStop() noexcept;
  bool stop_requested() const noexcept;

 private:
  std::atomic<std::uint32_t> requested_{0U};
};

// The application actor is the only writer and the capture worker is the only
// reader. Epoch changes invalidate an in-flight wait within wait_slice_ms.
class CaptureEpochFence final {
  static_assert(std::atomic<std::uint32_t>::is_always_lock_free,
                "capture epoch fence requires lock-free 32-bit atomics");

 public:
  Status AdvanceTo(std::uint32_t epoch);
  std::uint32_t Observe() const noexcept;

 private:
  std::atomic<std::uint32_t> epoch_{0U};
};

struct AudioPeriodTransferConfig final {
  std::uint32_t wait_slice_ms{10U};
  std::uint32_t operation_timeout_ms{200U};
};

enum class AudioIoCancelCode : std::uint8_t {
  kAcknowledged = 0,
  kNotActive,
  kBackendFault,
};

struct AudioIoCancelResult final {
  AudioIoCancelCode code{AudioIoCancelCode::kBackendFault};

  constexpr bool acknowledged() const noexcept {
    return code == AudioIoCancelCode::kAcknowledged;
  }
};

// RV1106 playback-device adapter for the portable PlaybackCommitter. It owns
// no generation, accepted-prefix, cancellation, or reference state. The
// committer remains the single source of truth for those contracts.
//
// The portable input is mono. This adapter duplicates it into the board's
// verified two-channel interleaved PCM boundary using fixed member storage.
// Every call is nonblocking and performs no allocation, logging, or waiting.
class PcmPlaybackSink48k final : public audio::PcmPlaybackSink {
 public:
  PcmPlaybackSink48k(PcmStreamBackend* backend,
                     const AudioIoClock* clock) noexcept;
  PcmPlaybackSink48k(const PcmPlaybackSink48k&) = delete;
  PcmPlaybackSink48k& operator=(const PcmPlaybackSink48k&) = delete;

  audio::PcmPlaybackWriteResult TryWriteInterleavedS16(
      const std::int16_t* samples,
      std::uint16_t frames) noexcept override;
  audio::PcmPlaybackControlResult Drop() noexcept override;
  audio::PcmPlaybackControlResult Prepare() noexcept override;

 private:
  PcmStreamBackend* backend_{nullptr};
  const AudioIoClock* clock_{nullptr};
  std::array<std::int16_t,
             audio::AcceptedRenderChunk48k::kSampleCapacity * 2U>
      stereo_scratch_{};
};

enum class CaptureTransferCode : std::uint8_t {
  kCaptured = 0,
  kInvalidArgument,
  kNotActive,
  kGenerationMismatch,
  kCancelled,
  kPartialDiscarded,
  kTimeout,
  kXrun,
  kSuspended,
  kBackendFault,
};

struct CaptureTransferResult final {
  CaptureTransferCode code{CaptureTransferCode::kBackendFault};
  std::uint32_t discarded_sample_frames{0U};

  constexpr bool captured() const noexcept {
    return code == CaptureTransferCode::kCaptured;
  }
};

// Capture-worker-owned reader. ALSA always writes into private scratch first;
// only an exact 960-frame read can publish a CaptureFrame. A known short read
// accounts one discarded logical period; xrun/timeout only mark discontinuity
// because their lost duration is unknown. Stale samples never escape a reused
// queue slot and sequence gaps are never fabricated.
class CapturePeriodReader48k final {
 public:
  CapturePeriodReader48k(PcmStreamBackend* backend,
                         const AudioIoClock* clock) noexcept;
  CapturePeriodReader48k(const CapturePeriodReader48k&) = delete;
  CapturePeriodReader48k& operator=(const CapturePeriodReader48k&) = delete;

  Status Configure(const AudioPeriodTransferConfig& config,
                   std::uint8_t capture_channels);
  Status Activate(std::uint32_t epoch, std::uint32_t stream_id,
                  const CaptureEpochFence& epoch_fence);
  AudioIoCancelResult Cancel() noexcept;

  CaptureTransferResult Read(std::uint32_t turn_id,
                             const CaptureEpochFence& epoch_fence,
                             const AudioIoStopFlag& stop_flag,
                             audio::CaptureFrame* output) noexcept;

 private:
  void AccountDiscardedPeriod() noexcept;
  void MarkDiscontinuity() noexcept;

  PcmStreamBackend* backend_{nullptr};
  const AudioIoClock* clock_{nullptr};
  AudioPeriodTransferConfig config_{};
  audio::AudioFrameSequencer sequencer_{};
  std::array<std::int16_t,
             audio::kCaptureFrameInterleavedSampleCapacity>
      capture_scratch_{};
  std::uint32_t active_epoch_{0U};
  std::uint32_t active_stream_id_{0U};
  std::uint32_t retired_epoch_{0U};
  std::uint32_t retired_stream_id_{0U};
  std::uint8_t capture_channels_{0U};
  bool configured_{false};
  bool active_{false};
};

}  // namespace boompi::platform

#endif  // BOOMPI_PLATFORM_AUDIO_PCM_TRANSFER_H_
