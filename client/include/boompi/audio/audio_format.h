#ifndef BOOMPI_AUDIO_AUDIO_FORMAT_H_
#define BOOMPI_AUDIO_AUDIO_FORMAT_H_

#include <cstdint>

#include "boompi/event/status.h"

namespace boompi::audio {

enum class SampleFormat : std::uint8_t {
  kPcmS16Le = 1,
};

struct AudioFormat final {
  std::uint32_t sample_rate_hz{0};
  std::uint16_t frame_duration_ms{0};
  std::uint8_t channels{0};
  SampleFormat sample_format{SampleFormat::kPcmS16Le};
};

struct AudioFrameMetadata final {
  std::uint64_t monotonic_timestamp_us{0};
  std::uint32_t sequence{0};
  std::uint32_t stream_id{0};
  std::uint32_t turn_id{0};
  std::uint32_t epoch{0};
  bool discontinuity{false};
};

Status ValidateAudioFormat(const AudioFormat& format);
std::uint32_t SamplesPerChannel(const AudioFormat& format);
std::uint32_t BytesPerFrame(const AudioFormat& format);

}  // namespace boompi::audio

#endif  // BOOMPI_AUDIO_AUDIO_FORMAT_H_
