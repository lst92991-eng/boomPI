#ifndef BOOMPI_AUDIO_AUDIO_FRAME_H_
#define BOOMPI_AUDIO_AUDIO_FRAME_H_

#include <array>
#include <cstddef>
#include <cstdint>

#include "boompi/audio/audio_format.h"

namespace boompi::audio {

constexpr std::size_t kCaptureFrameInterleavedSampleCapacity = 3840U;
constexpr std::size_t kMono16kFrameInterleavedSampleCapacity = 320U;

struct CaptureFrameContract final {
  static bool Matches(const AudioFormat& format,
                      std::uint32_t samples_per_channel) noexcept {
    return format.sample_rate_hz == 48000U &&
           format.frame_duration_ms == 20U &&
           (format.channels == 2U || format.channels == 4U) &&
           samples_per_channel == 960U;
  }
};

struct Mono16kFrameContract final {
  static bool Matches(const AudioFormat& format,
                      std::uint32_t samples_per_channel) noexcept {
    return format.sample_rate_hz == 16000U &&
           format.frame_duration_ms == 20U && format.channels == 1U &&
           samples_per_channel == 320U;
  }
};

// PcmFrame owns a fixed-size sample buffer so that audio workers never need to
// allocate memory per frame. HasValidLength validates the declared extent, but
// cannot prove that a producer wrote every sample. A partial ALSA period must
// cancel the write lease; producers set samples_per_channel only after fully
// overwriting the declared range. Stale samples beyond that range are invalid.
template <std::size_t MaxInterleavedSamples, typename Contract>
struct PcmFrame final {
  static_assert(MaxInterleavedSamples > 0U,
                "an audio frame must hold at least one sample");

  AudioFormat format{};
  AudioFrameMetadata metadata{};
  std::uint32_t samples_per_channel{0U};
  std::array<std::int16_t, MaxInterleavedSamples> samples{};

  static constexpr std::size_t sample_capacity() noexcept {
    return MaxInterleavedSamples;
  }

  bool HasValidLength() const noexcept {
    if (format.sample_rate_hz < 8000U || format.sample_rate_hz > 96000U ||
        format.frame_duration_ms == 0U ||
        format.frame_duration_ms > 1000U || format.channels == 0U ||
        format.channels > 8U ||
        format.sample_format != SampleFormat::kPcmS16Le) {
      return false;
    }
    const auto sample_product =
        static_cast<std::uint64_t>(format.sample_rate_hz) *
        static_cast<std::uint64_t>(format.frame_duration_ms);
    if (sample_product % 1000U != 0U ||
        samples_per_channel != sample_product / 1000U) {
      return false;
    }
    const auto interleaved_sample_count =
        static_cast<std::uint64_t>(samples_per_channel) *
        static_cast<std::uint64_t>(format.channels);
    return interleaved_sample_count <=
               static_cast<std::uint64_t>(MaxInterleavedSamples) &&
           Contract::Matches(format, samples_per_channel);
  }

  std::size_t interleaved_samples() const noexcept {
    if (!HasValidLength()) {
      return 0U;
    }
    return static_cast<std::size_t>(samples_per_channel) *
           static_cast<std::size_t>(format.channels);
  }

  // Header reset intentionally leaves PCM storage untouched. The producer
  // overwrites exactly samples_per_channel * channels samples before publish.
  void ResetHeader() noexcept {
    format = AudioFormat{};
    metadata = AudioFrameMetadata{};
    samples_per_channel = 0U;
  }
};

using CaptureFrame =
    PcmFrame<kCaptureFrameInterleavedSampleCapacity, CaptureFrameContract>;
using Mono16kFrame =
    PcmFrame<kMono16kFrameInterleavedSampleCapacity, Mono16kFrameContract>;

// AudioFrameSequencer belongs exclusively to one producer thread. It accounts
// for frames discarded before publication so the next visible frame carries
// both a sequence gap and an explicit discontinuity marker.
class AudioFrameSequencer final {
 public:
  // Reset starts a different generation. Reusing the active epoch/stream pair
  // is rejected so sequence reset cannot silently erase a discontinuity.
  bool Reset(std::uint32_t epoch, std::uint32_t stream_id,
             std::uint32_t next_sequence = 0U) noexcept;

  AudioFrameMetadata NextPublished(std::uint64_t monotonic_timestamp_us,
                                   std::uint32_t turn_id) noexcept;
  void AccountDroppedFrame() noexcept;
  void MarkDiscontinuity() noexcept;
  bool initialized() const noexcept;

 private:
  std::uint32_t epoch_{0U};
  std::uint32_t stream_id_{0U};
  std::uint32_t next_sequence_{0U};
  bool discontinuity_pending_{false};
};

}  // namespace boompi::audio

#endif  // BOOMPI_AUDIO_AUDIO_FRAME_H_
