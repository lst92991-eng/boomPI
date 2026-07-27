#ifndef BOOMPI_PLATFORM_AUDIO_PCM_PLAYBACK_H_
#define BOOMPI_PLATFORM_AUDIO_PCM_PLAYBACK_H_

#include <array>
#include <cstdint>

#include "boompi/audio/pcm_playback_sink.h"
#include "boompi/event/status.h"

namespace boompi::platform {

// Typed result shared by the platform-independent sink adapter and a concrete
// playback device. kSucceeded has operation-specific payload semantics; every
// other code carries no trusted payload, but may retain a native error value.
enum class PcmPlaybackDeviceCode : std::uint8_t {
  kUnset = 0,
  kSucceeded,
  kWouldBlock,
  kInterrupted,
  kXrun,
  kSuspended,
  kDeviceLost,
  kZeroProgress,
  kFatal,
  kUnavailable,
};

struct PcmPlaybackDeviceConfig final {
  std::uint32_t sample_rate_hz{0U};
  std::uint32_t period_sample_frames{0U};
  std::uint32_t buffer_sample_frames{0U};
  std::uint32_t start_threshold_sample_frames{0U};
  std::uint32_t avail_min_sample_frames{0U};
  std::uint8_t channels{0U};

  constexpr bool valid() const noexcept {
    return sample_rate_hz != 0U && (channels == 1U || channels == 2U) &&
           period_sample_frames != 0U &&
           period_sample_frames <= buffer_sample_frames / 2U &&
           buffer_sample_frames <=
               audio::PcmPlaybackWriteResult::kMaximumQueuedSampleFrames &&
           start_threshold_sample_frames != 0U &&
           start_threshold_sample_frames <=
               audio::AcceptedRenderChunk48k::kSampleCapacity &&
           start_threshold_sample_frames <= buffer_sample_frames &&
           avail_min_sample_frames != 0U &&
           avail_min_sample_frames <= buffer_sample_frames;
  }
};

struct PcmPlaybackDeviceWriteResult final {
  PcmPlaybackDeviceCode code{PcmPlaybackDeviceCode::kUnset};
  std::uint32_t accepted_sample_frames{0U};
  std::int32_t native_error{0};
};

enum class PcmPlaybackTimelineState : std::uint8_t {
  kUnset = 0,
  kPrepared,
  kRunning,
};

// Snapshot taken immediately before or after a write. The device also owns the
// clock accessor, so its status timestamp and NowUs must share one monotonic
// epoch. queued_sample_frames is ALSA's current playback delay estimate.
struct PcmPlaybackDeviceStatusResult final {
  PcmPlaybackDeviceCode code{PcmPlaybackDeviceCode::kUnset};
  std::uint32_t queued_sample_frames{0U};
  std::uint64_t monotonic_timestamp_us{0U};
  std::int32_t native_error{0};
  PcmPlaybackTimelineState timeline_state{PcmPlaybackTimelineState::kUnset};
};

struct PcmPlaybackDeviceControlResult final {
  PcmPlaybackDeviceCode code{PcmPlaybackDeviceCode::kUnset};
  std::int32_t native_error{0};
};

// One concrete device is exclusively owned and called by the playback thread.
// All methods are bounded and nonblocking. Recovery is deliberately absent:
// xrun/suspend teardown remains the PlaybackCommitter Drop -> Prepare
// transaction so a PCM timeline can never change behind its incarnation.
class PcmPlaybackDevice {
public:
  PcmPlaybackDevice() noexcept = default;
  PcmPlaybackDevice(const PcmPlaybackDevice &) = delete;
  PcmPlaybackDevice &operator=(const PcmPlaybackDevice &) = delete;
  PcmPlaybackDevice(PcmPlaybackDevice &&) = delete;
  PcmPlaybackDevice &operator=(PcmPlaybackDevice &&) = delete;
  virtual ~PcmPlaybackDevice() noexcept = default;

  virtual PcmPlaybackDeviceConfig config() const noexcept = 0;
  virtual std::uint64_t MonotonicNowUs() const noexcept = 0;
  virtual PcmPlaybackDeviceStatusResult QueryStatus() noexcept = 0;
  virtual PcmPlaybackDeviceWriteResult
  TryWriteInterleavedS16(const std::int16_t *samples,
                         std::uint32_t sample_frames) noexcept = 0;
  virtual PcmPlaybackDeviceControlResult Drop() noexcept = 0;
  virtual PcmPlaybackDeviceControlResult Prepare() noexcept = 0;
};

// Adapts the renderer's fixed 48 kHz mono S16 boundary to an explicitly
// configured one- or two-channel playback device. Stereo output duplicates
// mono into fixed member scratch. The hot path allocates nothing, does not
// wait, and never retains the caller's samples.
class PcmPlaybackSink48k final : public audio::PcmPlaybackSink {
public:
  PcmPlaybackSink48k() noexcept = default;
  PcmPlaybackSink48k(const PcmPlaybackSink48k &) = delete;
  PcmPlaybackSink48k &operator=(const PcmPlaybackSink48k &) = delete;
  PcmPlaybackSink48k(PcmPlaybackSink48k &&) = delete;
  PcmPlaybackSink48k &operator=(PcmPlaybackSink48k &&) = delete;

  static Status Create(PcmPlaybackDevice *device, PcmPlaybackSink48k *output);

  audio::PcmPlaybackWriteResult
  TryWriteInterleavedS16(const std::int16_t *samples,
                         std::uint16_t frames) noexcept override;
  audio::PcmPlaybackControlResult Drop() noexcept override;
  audio::PcmPlaybackControlResult Prepare() noexcept override;

  bool configured() const noexcept { return device_ != nullptr; }

private:
  PcmPlaybackDevice *device_{nullptr};
  std::uint8_t device_channels_{0U};
  std::array<std::int16_t, audio::AcceptedRenderChunk48k::kSampleCapacity * 2U>
      interleaved_scratch_{};
};

} // namespace boompi::platform

#endif // BOOMPI_PLATFORM_AUDIO_PCM_PLAYBACK_H_
