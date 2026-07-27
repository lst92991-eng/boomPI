#ifndef BOOMPI_AUDIO_AUDIO_DSP_TYPES_H_
#define BOOMPI_AUDIO_AUDIO_DSP_TYPES_H_

#include <array>
#include <cstddef>
#include <cstdint>

namespace boompi::audio {

constexpr std::size_t kDspCaptureChannelCount = 4U;
constexpr std::size_t kSamplesPer48k20ms = 960U;
constexpr std::size_t kSamplesPer16k20ms = 320U;

using CapturePlane48k = std::array<std::int16_t, kSamplesPer48k20ms>;
using CapturePlane16k = std::array<std::int16_t, kSamplesPer16k20ms>;
using CapturePlanes48k =
    std::array<CapturePlane48k, kDspCaptureChannelCount>;
using CapturePlanes16k =
    std::array<CapturePlane16k, kDspCaptureChannelCount>;

enum class SampleTransformCode : std::uint8_t {
  kOk = 0,
  kInvalidArgument,
};

struct SampleTransformResult final {
  SampleTransformCode code{SampleTransformCode::kInvalidArgument};
  std::uint32_t saturated_samples{0U};

  bool ok() const noexcept { return code == SampleTransformCode::kOk; }
};

}  // namespace boompi::audio

#endif  // BOOMPI_AUDIO_AUDIO_DSP_TYPES_H_
