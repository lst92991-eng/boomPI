#ifndef BOOMPI_AUDIO_PLAYBACK_FRAME_H_
#define BOOMPI_AUDIO_PLAYBACK_FRAME_H_

#include <array>
#include <cstddef>
#include <cstdint>

#include "boompi/audio/audio_format.h"

namespace boompi::audio {

// How the local first-sample presentation timestamp was obtained. Both valid
// sources are estimates until their relationship with the actual Codec/DAC is
// measured on RV1106 hardware.
enum class PlaybackTimingSource : std::uint8_t {
  kUnset = 0,
  kSoftwareEstimate,
  kAlsaStatus,
};

// A network worker produces fixed 20 ms TTS frames for one playback worker.
// The final provider frame may contain fewer than 480 source samples, but its
// unused tail must be zero so a reused queue slot can never replay stale PCM.
struct TtsPcmFrame24k final {
  static constexpr std::uint32_t kSampleRateHz = 24000U;
  static constexpr std::uint16_t kFrameDurationMs = 20U;
  static constexpr std::uint8_t kChannelCount = 1U;
  static constexpr std::uint16_t kFrameSamples = 480U;
  static constexpr SampleFormat kSampleFormat = SampleFormat::kPcmS16Le;

  AudioFormat format{};
  AudioFrameMetadata metadata{};
  std::uint16_t valid_source_samples{0U};
  bool end_of_stream{false};
  std::array<std::int16_t, kFrameSamples> samples{};

  static constexpr std::size_t sample_capacity() noexcept {
    return kFrameSamples;
  }

  bool HasValidLength() const noexcept;

  // Header reset intentionally preserves PCM storage. A producer must
  // overwrite all 480 samples, or overwrite the valid prefix and zero the
  // remaining tail, before publishing the lease.
  void ResetHeader() noexcept;
};

// Wide signed PCM sample units produced by the FIR interpolator. Values may
// exceed S16 during a valid band-limited overshoot, so this internal boundary
// deliberately has no wire/ALSA AudioFormat and must never be cast directly
// to S16. Gain, duck, and peak limiting consume it before the final S16 frame.
struct ResampledPcmFrame48k final {
  static constexpr std::uint32_t kSampleRateHz = 48000U;
  static constexpr std::uint16_t kFrameDurationMs = 20U;
  static constexpr std::uint8_t kChannelCount = 1U;
  static constexpr std::uint16_t kFrameSamples = 960U;
  static constexpr std::uint16_t kMaximumSourceOffsetSampleFrames = 960U;
  static constexpr std::uint16_t kMaximumSourceSpanSampleFrames = 1023U;

  AudioFrameMetadata metadata{};
  std::uint16_t source_offset_sample_frames{0U};
  std::uint16_t valid_samples{0U};
  bool end_of_stream{false};
  std::array<std::int32_t, kFrameSamples> samples{};

  static constexpr std::size_t sample_capacity() noexcept {
    return kFrameSamples;
  }

  bool HasValidLength() const noexcept;
  void ResetHeader() noexcept;
};

// One bounded software-rendered mono PCM chunk after 24 -> 48 kHz conversion
// and before any ALSA write. This type carries no claim that a sample reached
// the kernel, Codec, speaker, or AEC reference path. A source frame can yield
// a prefix and an FIR-drain chunk with the same metadata; source_offset lets
// downstream code distinguish them. Every unused sample must be zero.
struct PlaybackPcmFrame48k final {
  static constexpr std::uint32_t kSampleRateHz = 48000U;
  static constexpr std::uint16_t kFrameDurationMs = 20U;
  static constexpr std::uint8_t kChannelCount = 1U;
  static constexpr std::uint16_t kFrameSamples = 960U;
  static constexpr std::uint16_t kMaximumSourceOffsetSampleFrames = 960U;
  static constexpr std::uint16_t kMaximumSourceSpanSampleFrames = 1023U;
  static constexpr SampleFormat kSampleFormat = SampleFormat::kPcmS16Le;

  AudioFormat format{};
  AudioFrameMetadata metadata{};
  std::uint16_t source_offset_sample_frames{0U};
  std::uint16_t valid_samples{0U};
  bool end_of_stream{false};
  std::array<std::int16_t, kFrameSamples> samples{};

  static constexpr std::size_t sample_capacity() noexcept {
    return kFrameSamples;
  }

  bool HasValidLength() const noexcept;

  // Header reset intentionally preserves PCM storage. A producer overwrites
  // the valid prefix and zeroes every unused tail sample before publication.
  void ResetHeader() noexcept;
};

struct RenderReferenceFrame48k final {
  static constexpr std::uint32_t kSampleRateHz = 48000U;
  static constexpr std::uint16_t kFrameDurationMs = 20U;
  static constexpr std::uint8_t kMaximumChannels = 2U;
  static constexpr std::uint32_t kSamplesPerChannel = 960U;
  static constexpr std::uint32_t kMaximumQueuedDurationMs = 1500U;
  static constexpr std::uint32_t kMaximumQueuedSampleFrames =
      (kSampleRateHz * kMaximumQueuedDurationMs) / 1000U;
  static constexpr std::size_t kInterleavedSampleCapacity =
      static_cast<std::size_t>(kSamplesPerChannel) * kMaximumChannels;
  static constexpr SampleFormat kSampleFormat = SampleFormat::kPcmS16Le;

  AudioFormat format{};
  // monotonic_timestamp_us is the estimated local presentation time of the
  // first sample in this frame, not its network receive or render-start time.
  AudioFrameMetadata metadata{};
  std::uint32_t samples_per_channel{0U};
  // Local software timestamp captured after the final PCM samples have been
  // rendered. It is not an ALSA completion or confirmed-played timestamp.
  std::uint64_t render_completed_timestamp_us{0U};
  std::uint32_t queued_sample_frames_before_write{0U};
  PlaybackTimingSource timing_source{PlaybackTimingSource::kUnset};
  std::array<std::int16_t, kInterleavedSampleCapacity> samples{};

  static constexpr std::size_t sample_capacity() noexcept {
    return kInterleavedSampleCapacity;
  }

  bool HasValidLength() const noexcept;
  std::size_t interleaved_samples() const noexcept;

  // This type represents one complete 20 ms render period. A partial ALSA
  // write must not be published as a RenderReferenceFrame48k: platform
  // integration must either complete the period or report the accepted
  // prefix through a separate bounded accepted-chunk contract before it can
  // claim complete playback-reference coverage.

  // Header reset intentionally preserves PCM storage. Only the declared
  // channel extent is valid after the producer fully overwrites and publishes
  // a new frame.
  void ResetHeader() noexcept;
};

}  // namespace boompi::audio

#endif  // BOOMPI_AUDIO_PLAYBACK_FRAME_H_
