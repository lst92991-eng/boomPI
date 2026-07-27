#include "boompi/audio/fir_decimator_48_to_16.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace boompi::audio {
namespace {

constexpr std::int64_t kQ15Scale = 32768;
constexpr std::int64_t kQ15Half = kQ15Scale / 2;

constexpr std::array<std::int16_t, 211U> kCoefficientsQ15{{
    0, 1, 1, -1, -2, -1, 0, 2,
    2, 0, -3, -4, -1, 4, 6, 2,
    -4, -8, -5, 3, 10, 8, -2, -12,
    -12, 0, 14, 16, 4, -15, -22, -9,
    14, 27, 16, -12, -33, -25, 7, 37,
    36, 0, -40, -47, -11, 41, 60, 25,
    -38, -72, -42, 31, 83, 63, -18, -91,
    -86, 0, 96, 112, 25, -95, -138, -57,
    86, 163, 95, -69, -186, -141, 41, 203,
    192, 0, -212, -248, -55, 210, 307, 126,
    -194, -369, -217, 158, 429, 329, -97, -488,
    -470, 0, 542, 651, 149, -590, -899, -388,
    629, 1280, 816, -658, -2031, -1835, 677, 4813,
    8670, 10250, 8670, 4813, 677, -1835, -2031, -658,
    816, 1280, 629, -388, -899, -590, 149, 651,
    542, 0, -470, -488, -97, 329, 429, 158,
    -217, -369, -194, 126, 307, 210, -55, -248,
    -212, 0, 192, 203, 41, -141, -186, -69,
    95, 163, 86, -57, -138, -95, 25, 112,
    96, 0, -86, -91, -18, 63, 83, 31,
    -42, -72, -38, 25, 60, 41, -11, -47,
    -40, 0, 36, 37, 7, -25, -33, -12,
    16, 27, 14, -9, -22, -15, 4, 16,
    14, 0, -12, -12, -2, 8, 10, 3,
    -5, -8, -4, 2, 6, 4, -1, -4,
    -3, 0, 2, 2, 0, -1, -2, -1,
    1, 1, 0
}};

constexpr bool CoefficientsAreValid() noexcept {
  std::int32_t sum = 0;
  for (std::size_t tap = 0U; tap < kCoefficientsQ15.size(); ++tap) {
    sum += kCoefficientsQ15[tap];
    if (kCoefficientsQ15[tap] !=
        kCoefficientsQ15[kCoefficientsQ15.size() - 1U - tap]) {
      return false;
    }
  }
  return sum == kQ15Scale;
}

static_assert(kCoefficientsQ15.size() == FirDecimator48To16::kTapCount,
              "FIR coefficient count changed");
static_assert(CoefficientsAreValid(),
              "FIR coefficients must be symmetric with unity DC gain");
static_assert(FirDecimator48To16::kLatencyInputSamples %
                      FirDecimator48To16::kDecimationFactor ==
                  0U,
              "FIR group delay must be integral at the output rate");
static_assert(kSamplesPer48k20ms ==
                  kSamplesPer16k20ms *
                      FirDecimator48To16::kDecimationFactor,
              "48 kHz to 16 kHz frames must have an exact 3:1 ratio");

std::int64_t RoundQ15(const std::int64_t accumulator) noexcept {
  if (accumulator >= 0) {
    return (accumulator + kQ15Half) / kQ15Scale;
  }
  return -((-accumulator + kQ15Half) / kQ15Scale);
}

std::int16_t SaturateS16(const std::int64_t value,
                         std::uint32_t* const saturation_count) noexcept {
  if (value > std::numeric_limits<std::int16_t>::max()) {
    ++(*saturation_count);
    return std::numeric_limits<std::int16_t>::max();
  }
  if (value < std::numeric_limits<std::int16_t>::min()) {
    ++(*saturation_count);
    return std::numeric_limits<std::int16_t>::min();
  }
  return static_cast<std::int16_t>(value);
}

}  // namespace

void FirDecimator48To16::Reset() noexcept {
  for (auto& channel_history : history_) {
    channel_history.fill(0);
  }
}

SampleTransformResult FirDecimator48To16::Process(
    const CapturePlanes48k& input, CapturePlanes16k* const output) noexcept {
  if (output == nullptr) {
    return {SampleTransformCode::kInvalidArgument, 0U};
  }

  std::uint32_t saturated_samples = 0U;
  for (std::size_t channel = 0U; channel < kDspCaptureChannelCount;
       ++channel) {
    for (std::size_t output_sample = 0U;
         output_sample < kSamplesPer16k20ms; ++output_sample) {
      const std::size_t input_sample =
          output_sample * FirDecimator48To16::kDecimationFactor;
      const std::size_t current_taps =
          input_sample + 1U < kTapCount ? input_sample + 1U : kTapCount;
      std::int64_t accumulator = 0;

      for (std::size_t tap = 0U; tap < current_taps; ++tap) {
        accumulator +=
            static_cast<std::int64_t>(kCoefficientsQ15[tap]) *
            static_cast<std::int64_t>(input[channel][input_sample - tap]);
      }
      for (std::size_t tap = current_taps; tap < kTapCount; ++tap) {
        const std::size_t history_sample =
            kHistorySamples + input_sample - tap;
        accumulator +=
            static_cast<std::int64_t>(kCoefficientsQ15[tap]) *
            static_cast<std::int64_t>(history_[channel][history_sample]);
      }

      (*output)[channel][output_sample] =
          SaturateS16(RoundQ15(accumulator), &saturated_samples);
    }

    for (std::size_t history_sample = 0U;
         history_sample < kHistorySamples; ++history_sample) {
      history_[channel][history_sample] =
          input[channel][kSamplesPer48k20ms - kHistorySamples +
                         history_sample];
    }
  }

  return {SampleTransformCode::kOk, saturated_samples};
}

}  // namespace boompi::audio
