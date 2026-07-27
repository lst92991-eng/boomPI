#ifndef BOOMPI_AUDIO_FIR_DECIMATOR_48_TO_16_H_
#define BOOMPI_AUDIO_FIR_DECIMATOR_48_TO_16_H_

#include <array>
#include <cstddef>
#include <cstdint>

#include "boompi/audio/audio_dsp_types.h"

namespace boompi::audio {

class FirDecimator48To16 final {
 public:
  static constexpr std::size_t kTapCount = 211U;
  static constexpr std::size_t kDecimationFactor = 3U;
  static constexpr std::size_t kHistorySamples = kTapCount - 1U;
  static constexpr std::size_t kLatencyInputSamples = (kTapCount - 1U) / 2U;
  static constexpr std::size_t kLatencyOutputSamples =
      kLatencyInputSamples / kDecimationFactor;

  FirDecimator48To16() noexcept = default;

  // One DSP worker exclusively owns an instance. Reset and Process are
  // serialized on that worker; a control-plane thread must request Reset via
  // the worker's control queue instead of calling it concurrently.
  void Reset() noexcept;
  SampleTransformResult Process(const CapturePlanes48k& input,
                                CapturePlanes16k* output) noexcept;

 private:
  std::array<std::array<std::int16_t, kHistorySamples>,
             kDspCaptureChannelCount>
      history_{};
};

}  // namespace boompi::audio

#endif  // BOOMPI_AUDIO_FIR_DECIMATOR_48_TO_16_H_
