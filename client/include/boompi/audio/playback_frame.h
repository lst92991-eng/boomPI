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

// One exact mono prefix accepted by a positive playback-device write. The
// source identity is the renderer metadata plus an absolute offset spanning
// its normal prefix and optional FIR drain. This record proves only device
// acceptance; its presentation timestamp remains an estimate and it never
// proves Codec/DAC output or audible speaker playback.
struct AcceptedRenderChunk48k final {
  static constexpr std::uint32_t kSampleRateHz = 48000U;
  static constexpr std::uint16_t kFrameDurationMs = 20U;
  static constexpr std::uint8_t kChannelCount = 1U;
  static constexpr std::uint16_t kSampleCapacity = 960U;
  static constexpr std::uint16_t kMaximumSourceSpanSampleFrames = 1023U;
  static constexpr std::uint32_t kMaximumQueuedDurationMs = 1500U;
  static constexpr std::uint32_t kMaximumQueuedSampleFrames =
      (kSampleRateHz * kMaximumQueuedDurationMs) / 1000U;
  static constexpr SampleFormat kSampleFormat = SampleFormat::kPcmS16Le;

  AudioFormat format{};
  // This remains the source render metadata. In particular, its timestamp is
  // not rewritten to mean write completion or estimated presentation.
  AudioFrameMetadata metadata{};
  // Non-zero identity for one prepared PCM timeline. The playback owner must
  // allocate a new value after drop/prepare, reopen, or any equivalent
  // timeline reset and must not reuse it during the process lifetime.
  std::uint64_t pcm_incarnation{0U};
  // Process-wide, non-zero sequence allocated only for positive accepted
  // writes. It must increase without reuse; exhaustion requires a quiescent
  // runtime restart rather than wraparound.
  std::uint64_t accepted_chunk_sequence{0U};
  // Absolute offset in the source render span identified by metadata. A
  // normal 20 ms prefix occupies 0..959 and an EOS FIR drain can extend the
  // same source span through offset 1022.
  std::uint16_t absolute_source_offset_sample_frames{0U};
  std::uint16_t accepted_samples{0U};
  // True only when this accepted prefix reaches the end of the particular
  // PlaybackPcmFrame48k passed to the device. A source EOS marker is legal
  // only on such a completing accepted prefix.
  bool completes_render_chunk{false};
  bool source_end_of_stream{false};
  // Local monotonic timestamp captured after the positive device write
  // returned. The presentation estimate is for samples[0], must not precede
  // write completion, and is qualified by timing_source.
  std::uint64_t write_completed_timestamp_us{0U};
  std::uint64_t estimated_presentation_timestamp_us{0U};
  std::uint32_t queued_sample_frames_before_write{0U};
  PlaybackTimingSource timing_source{PlaybackTimingSource::kUnset};
  std::array<std::int16_t, kSampleCapacity> samples{};

  static constexpr std::size_t sample_capacity() noexcept {
    return kSampleCapacity;
  }

  bool HasValidLength() const noexcept;

  // Header reset intentionally preserves PCM storage. A producer must copy
  // exactly the accepted prefix and zero every unused tail sample before
  // publishing a queue lease.
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
