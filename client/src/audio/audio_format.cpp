#include "boompi/audio/audio_format.h"

#include <limits>

namespace boompi::audio {

Status ValidateAudioFormat(const AudioFormat& format) {
  if (format.sample_rate_hz < 8000U || format.sample_rate_hz > 96000U) {
    return Status::Error(StatusCode::kInvalidArgument,
                         "sample rate must be between 8000 and 96000 Hz");
  }
  if (format.frame_duration_ms == 0U || format.frame_duration_ms > 1000U) {
    return Status::Error(StatusCode::kInvalidArgument,
                         "frame duration must be between 1 and 1000 ms");
  }
  const auto sample_product =
      static_cast<std::uint64_t>(format.sample_rate_hz) *
      static_cast<std::uint64_t>(format.frame_duration_ms);
  if (sample_product % 1000U != 0U) {
    return Status::Error(StatusCode::kInvalidArgument,
                         "frame duration does not yield integral samples");
  }
  if (format.channels == 0U || format.channels > 8U) {
    return Status::Error(StatusCode::kInvalidArgument,
                         "channel count must be between 1 and 8");
  }
  if (format.sample_format != SampleFormat::kPcmS16Le) {
    return Status::Error(StatusCode::kNotSupported,
                         "only PCM S16_LE is supported in protocol v1");
  }
  return Status::Ok();
}

std::uint32_t SamplesPerChannel(const AudioFormat& format) {
  if (!ValidateAudioFormat(format).ok()) {
    return 0U;
  }
  return static_cast<std::uint32_t>(
      (static_cast<std::uint64_t>(format.sample_rate_hz) *
       static_cast<std::uint64_t>(format.frame_duration_ms)) /
      1000U);
}

std::uint32_t BytesPerFrame(const AudioFormat& format) {
  const auto samples_per_channel = SamplesPerChannel(format);
  if (samples_per_channel == 0U) {
    return 0U;
  }

  constexpr std::uint32_t kBytesPerS16Sample = 2U;
  const auto bytes = static_cast<std::uint64_t>(samples_per_channel) *
                     static_cast<std::uint64_t>(format.channels) *
                     kBytesPerS16Sample;
  if (bytes > std::numeric_limits<std::uint32_t>::max()) {
    return 0U;
  }
  return static_cast<std::uint32_t>(bytes);
}

}  // namespace boompi::audio
