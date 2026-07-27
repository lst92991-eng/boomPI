#ifndef BOOMPI_AUDIO_CAPTURE_CHANNEL_MAPPER_H_
#define BOOMPI_AUDIO_CAPTURE_CHANNEL_MAPPER_H_

#include <array>
#include <cstddef>
#include <cstdint>

#include "boompi/audio/audio_dsp_types.h"
#include "boompi/audio/audio_frame.h"
#include "boompi/event/status.h"

namespace boompi::audio {

enum class CaptureRole : std::uint8_t {
  kMicLeft = 0,
  kMicRight,
  kReferenceLeft,
  kReferenceRight,
};

struct ChannelRoute final {
  std::uint8_t input_slot{0U};
  std::int8_t polarity{1};
};

struct ChannelMap final {
  // routes is indexed by CaptureRole in declaration order. Each entry selects
  // one physical slot from an interleaved four-channel capture frame.
  std::array<ChannelRoute, kDspCaptureChannelCount> routes{};

  ChannelRoute& operator[](const CaptureRole role) noexcept {
    return routes[static_cast<std::size_t>(role)];
  }

  const ChannelRoute& operator[](const CaptureRole role) const noexcept {
    return routes[static_cast<std::size_t>(role)];
  }
};

class CaptureChannelMapper final {
 public:
  CaptureChannelMapper() = default;

  // One DSP worker exclusively owns an instance. Create is a cold-path
  // operation used before that worker starts or after it has stopped; it must
  // never run concurrently with Process. A failed update leaves output
  // unchanged.
  static Status Create(const ChannelMap& channel_map,
                       CaptureChannelMapper* output);

  // Process is allocation-free and accepts only a complete 48 kHz, 20 ms,
  // four-channel S16_LE capture frame.
  SampleTransformResult Process(const CaptureFrame& input,
                                CapturePlanes48k* output) const noexcept;

 private:
  ChannelMap channel_map_{};
  bool configured_{false};
};

}  // namespace boompi::audio

#endif  // BOOMPI_AUDIO_CAPTURE_CHANNEL_MAPPER_H_
