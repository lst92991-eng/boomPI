#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>

#include "boompi/audio/fir_decimator_48_to_16.h"
#include "boompi/test/test_context.h"

#define BOOMPI_EXPECT(context, expression) \
  (context).Expect((expression), #expression, __FILE__, __LINE__)

namespace {

constexpr double kPi = 3.141592653589793238462643383279502884;
constexpr std::int64_t kQ15Scale = 32768;
constexpr std::size_t kTwoFrameInputSamples =
    boompi::audio::kSamplesPer48k20ms * 2U;
using ReferenceCoefficients =
    std::array<std::int16_t, boompi::audio::FirDecimator48To16::kTapCount>;
using TwoFrameInput =
    std::array<std::array<std::int16_t, kTwoFrameInputSamples>,
               boompi::audio::kDspCaptureChannelCount>;

double BesselI0(const double value) {
  double term = 1.0;
  double total = 1.0;
  const double scaled = value * value / 4.0;
  for (std::size_t index = 1U; index < 100U; ++index) {
    const double divisor = static_cast<double>(index * index);
    term *= scaled / divisor;
    total += term;
    if (std::abs(term) <= std::abs(total) * 1.0e-18) {
      break;
    }
  }
  return total;
}

ReferenceCoefficients GenerateReferenceCoefficients() {
  constexpr std::size_t kTapCount =
      boompi::audio::FirDecimator48To16::kTapCount;
  constexpr std::size_t kMidpoint = (kTapCount - 1U) / 2U;
  constexpr double kNormalizedCutoff = 7500.0 / 48000.0;
  constexpr double kKaiserBeta = 6.76;
  std::array<double, kTapCount> floating{};
  const double window_denominator = BesselI0(kKaiserBeta);

  for (std::size_t tap = 0U; tap <= kMidpoint; ++tap) {
    const auto offset = static_cast<std::int32_t>(tap) -
                        static_cast<std::int32_t>(kMidpoint);
    double ideal = 2.0 * kNormalizedCutoff;
    if (offset != 0) {
      const double offset_value = static_cast<double>(offset);
      ideal = std::sin(2.0 * kPi * kNormalizedCutoff * offset_value) /
              (kPi * offset_value);
    }
    const double window_position =
        2.0 * static_cast<double>(tap) /
            static_cast<double>(kTapCount - 1U) -
        1.0;
    const double window =
        BesselI0(kKaiserBeta *
                 std::sqrt(std::max(
                     0.0, 1.0 - window_position * window_position))) /
        window_denominator;
    floating[tap] = ideal * window;
    floating[kTapCount - 1U - tap] = floating[tap];
  }

  double normalization = 0.0;
  for (const double coefficient : floating) {
    normalization += coefficient;
  }
  for (double& coefficient : floating) {
    coefficient /= normalization;
  }

  ReferenceCoefficients coefficients{};
  for (std::size_t tap = 0U; tap < kMidpoint; ++tap) {
    const auto quantized = static_cast<std::int16_t>(
        std::lround(floating[tap] * static_cast<double>(kQ15Scale)));
    coefficients[tap] = quantized;
    coefficients[kTapCount - 1U - tap] = quantized;
  }
  coefficients[kMidpoint] = static_cast<std::int16_t>(
      std::lround(floating[kMidpoint] * static_cast<double>(kQ15Scale)));

  std::int32_t sum = 0;
  for (const std::int16_t coefficient : coefficients) {
    sum += coefficient;
  }
  coefficients[kMidpoint] = static_cast<std::int16_t>(
      static_cast<std::int32_t>(coefficients[kMidpoint]) +
      static_cast<std::int32_t>(kQ15Scale) - sum);
  return coefficients;
}

std::int64_t RoundQ15Reference(const std::int64_t accumulator) {
  constexpr std::int64_t kHalf = kQ15Scale / 2;
  if (accumulator >= 0) {
    return (accumulator + kHalf) / kQ15Scale;
  }
  return -((-accumulator + kHalf) / kQ15Scale);
}

std::int16_t SaturateReference(const std::int64_t value) {
  if (value > std::numeric_limits<std::int16_t>::max()) {
    return std::numeric_limits<std::int16_t>::max();
  }
  if (value < std::numeric_limits<std::int16_t>::min()) {
    return std::numeric_limits<std::int16_t>::min();
  }
  return static_cast<std::int16_t>(value);
}

bool AllSamplesEqual(const boompi::audio::CapturePlanes16k& planes,
                     const std::array<std::int16_t,
                                      boompi::audio::kDspCaptureChannelCount>&
                         expected) {
  for (std::size_t channel = 0U;
       channel < boompi::audio::kDspCaptureChannelCount; ++channel) {
    for (const std::int16_t sample : planes[channel]) {
      if (sample != expected[channel]) {
        return false;
      }
    }
  }
  return true;
}

bool PlanesEqual(const boompi::audio::CapturePlanes16k& lhs,
                 const boompi::audio::CapturePlanes16k& rhs) {
  return lhs == rhs;
}

void TestZeroAndDc(boompi::test::TestContext& context) {
  boompi::audio::FirDecimator48To16 decimator;
  boompi::audio::CapturePlanes48k input{};
  boompi::audio::CapturePlanes16k output{};
  auto result = decimator.Process(input, &output);
  BOOMPI_EXPECT(context, result.ok());
  BOOMPI_EXPECT(context, result.saturated_samples == 0U);
  BOOMPI_EXPECT(context, AllSamplesEqual(output, {0, 0, 0, 0}));

  const std::array<std::int16_t, boompi::audio::kDspCaptureChannelCount>
      dc_values{{32767, -32768, 12345, -12345}};
  for (std::size_t channel = 0U; channel < dc_values.size(); ++channel) {
    input[channel].fill(dc_values[channel]);
  }
  decimator.Reset();
  decimator.Process(input, &output);
  result = decimator.Process(input, &output);
  BOOMPI_EXPECT(context, result.ok());
  BOOMPI_EXPECT(context, result.saturated_samples == 0U);
  BOOMPI_EXPECT(context, AllSamplesEqual(output, dc_values));
}

void TestCrossFrameImpulse(boompi::test::TestContext& context) {
  const auto coefficients = GenerateReferenceCoefficients();
  boompi::audio::FirDecimator48To16 decimator;
  boompi::audio::CapturePlanes48k input{};
  boompi::audio::CapturePlanes16k first_output{};
  boompi::audio::CapturePlanes16k second_output{};
  input[0][957U] = 32767;
  BOOMPI_EXPECT(context, decimator.Process(input, &first_output).ok());
  input = {};
  BOOMPI_EXPECT(context, decimator.Process(input, &second_output).ok());

  std::size_t peak_index = 0U;
  std::int32_t peak_magnitude = -1;
  for (std::size_t index = 0U; index < second_output[0].size(); ++index) {
    const auto sample = static_cast<std::int32_t>(second_output[0][index]);
    const auto magnitude = sample < 0 ? -sample : sample;
    if (magnitude > peak_magnitude) {
      peak_magnitude = magnitude;
      peak_index = index;
    }
  }
  const auto expected_peak = SaturateReference(RoundQ15Reference(
      static_cast<std::int64_t>(coefficients[105U]) * 32767));
  BOOMPI_EXPECT(context, peak_index == 34U);
  BOOMPI_EXPECT(context, second_output[0][34U] == expected_peak);
  bool unused_channels_are_zero = true;
  for (std::size_t channel = 1U;
       channel < boompi::audio::kDspCaptureChannelCount; ++channel) {
    for (const std::int16_t sample : second_output[channel]) {
      unused_channels_are_zero = unused_channels_are_zero && sample == 0;
    }
  }
  BOOMPI_EXPECT(context, unused_channels_are_zero);
}

void TestAgainstReferenceConvolution(boompi::test::TestContext& context) {
  const auto coefficients = GenerateReferenceCoefficients();
  TwoFrameInput stream{};
  std::uint32_t random_state = 0x12345678U;
  for (auto& channel : stream) {
    for (auto& sample : channel) {
      random_state = random_state * 1664525U + 1013904223U;
      const auto centered =
          static_cast<std::int32_t>((random_state >> 8U) % 20001U) - 10000;
      sample = static_cast<std::int16_t>(centered);
    }
  }

  boompi::audio::FirDecimator48To16 decimator;
  std::array<boompi::audio::CapturePlanes16k, 2U> actual{};
  for (std::size_t frame = 0U; frame < actual.size(); ++frame) {
    boompi::audio::CapturePlanes48k input{};
    for (std::size_t channel = 0U;
         channel < boompi::audio::kDspCaptureChannelCount; ++channel) {
      for (std::size_t sample = 0U; sample < input[channel].size(); ++sample) {
        input[channel][sample] =
            stream[channel][frame * boompi::audio::kSamplesPer48k20ms +
                            sample];
      }
    }
    const auto result = decimator.Process(input, &actual[frame]);
    BOOMPI_EXPECT(context, result.ok());
    BOOMPI_EXPECT(context, result.saturated_samples == 0U);
  }

  bool matches = true;
  for (std::size_t channel = 0U;
       channel < boompi::audio::kDspCaptureChannelCount; ++channel) {
    for (std::size_t output_sample = 0U;
         output_sample < boompi::audio::kSamplesPer16k20ms * 2U;
         ++output_sample) {
      const std::size_t input_sample =
          output_sample *
          boompi::audio::FirDecimator48To16::kDecimationFactor;
      std::int64_t accumulator = 0;
      for (std::size_t tap = 0U; tap < coefficients.size(); ++tap) {
        if (tap <= input_sample) {
          accumulator +=
              static_cast<std::int64_t>(coefficients[tap]) *
              stream[channel][input_sample - tap];
        }
      }
      const auto expected =
          SaturateReference(RoundQ15Reference(accumulator));
      const auto frame = output_sample / boompi::audio::kSamplesPer16k20ms;
      const auto frame_sample =
          output_sample % boompi::audio::kSamplesPer16k20ms;
      if (actual[frame][channel][frame_sample] != expected) {
        matches = false;
      }
    }
  }
  BOOMPI_EXPECT(context, matches);
}

double MeasureGain(const double frequency_hz) {
  constexpr std::size_t kFrameCount = 12U;
  constexpr std::size_t kWarmupFrames = 2U;
  constexpr double kAmplitude = 20000.0;
  // A non-zero phase prevents a decimated sine from accidentally landing on
  // zero crossings and making a stopband sample look better than it is.
  constexpr double kPhaseRadians = 0.371;
  boompi::audio::FirDecimator48To16 decimator;
  double input_energy = 0.0;
  double output_energy = 0.0;
  std::size_t input_count = 0U;
  std::size_t output_count = 0U;

  for (std::size_t frame = 0U; frame < kFrameCount; ++frame) {
    boompi::audio::CapturePlanes48k input{};
    for (std::size_t sample = 0U;
         sample < boompi::audio::kSamplesPer48k20ms; ++sample) {
      const std::size_t global_sample =
          frame * boompi::audio::kSamplesPer48k20ms + sample;
      const auto value = static_cast<std::int16_t>(std::lround(
          kAmplitude * std::sin(2.0 * kPi * frequency_hz *
                                static_cast<double>(global_sample) /
                                48000.0 +
                                kPhaseRadians)));
      for (auto& channel : input) {
        channel[sample] = value;
      }
      if (frame >= kWarmupFrames) {
        const double input_value = static_cast<double>(value);
        input_energy += input_value * input_value;
        ++input_count;
      }
    }
    boompi::audio::CapturePlanes16k output{};
    if (!decimator.Process(input, &output).ok()) {
      return -1.0;
    }
    if (frame >= kWarmupFrames) {
      for (const std::int16_t value : output[0]) {
        const double output_value = static_cast<double>(value);
        output_energy += output_value * output_value;
        ++output_count;
      }
    }
  }
  const double input_rms =
      std::sqrt(input_energy / static_cast<double>(input_count));
  const double output_rms =
      std::sqrt(output_energy / static_cast<double>(output_count));
  return output_rms / input_rms;
}

void TestHalfLsbRoundingSymmetry(boompi::test::TestContext& context) {
  // Coefficient h[1] is exactly one Q15 unit. Placing +/-16384 at x[2]
  // makes y[1]'s accumulator exactly +/-0.5 LSB and isolates the signed tie
  // rule from all other taps.
  boompi::audio::FirDecimator48To16 positive_decimator;
  boompi::audio::CapturePlanes48k input{};
  boompi::audio::CapturePlanes16k output{};
  input[0U][2U] = 16384;
  BOOMPI_EXPECT(context, positive_decimator.Process(input, &output).ok());
  BOOMPI_EXPECT(context, output[0U][1U] == 1);

  boompi::audio::FirDecimator48To16 negative_decimator;
  input = {};
  output = {};
  input[0U][2U] = -16384;
  BOOMPI_EXPECT(context, negative_decimator.Process(input, &output).ok());
  BOOMPI_EXPECT(context, output[0U][1U] == -1);
}

void TestFrequencyResponse(boompi::test::TestContext& context) {
  for (const double frequency_hz : {1000.0, 7000.0}) {
    const double gain = MeasureGain(frequency_hz);
    const double gain_db = 20.0 * std::log10(gain);
    BOOMPI_EXPECT(context, std::abs(gain_db) <= 0.05);
  }

  constexpr double kMaximumStopbandGain = 0.0012589254117941673;  // -58 dB.
  for (const double frequency_hz :
       {8000.0, 8250.0, 10000.0, 14000.0, 16000.0, 20000.0}) {
    BOOMPI_EXPECT(context,
                  MeasureGain(frequency_hz) <= kMaximumStopbandGain);
  }
}

void TestResetAndIdenticalChannels(boompi::test::TestContext& context) {
  boompi::audio::FirDecimator48To16 decimator;
  boompi::audio::CapturePlanes48k input{};
  for (std::size_t sample = 0U; sample < input[0].size(); ++sample) {
    const auto value = static_cast<std::int16_t>(
        static_cast<std::int32_t>((sample * 73U) % 20001U) - 10000);
    for (auto& channel : input) {
      channel[sample] = value;
    }
  }
  boompi::audio::CapturePlanes16k output{};
  BOOMPI_EXPECT(context, decimator.Process(input, &output).ok());
  bool identical = true;
  for (std::size_t channel = 1U;
       channel < boompi::audio::kDspCaptureChannelCount; ++channel) {
    identical = identical && output[channel] == output[0];
  }
  BOOMPI_EXPECT(context, identical);

  decimator.Reset();
  input = {};
  BOOMPI_EXPECT(context, decimator.Process(input, &output).ok());
  BOOMPI_EXPECT(context, AllSamplesEqual(output, {0, 0, 0, 0}));
}

void TestSaturation(boompi::test::TestContext& context) {
  const auto coefficients = GenerateReferenceCoefficients();
  boompi::audio::FirDecimator48To16 decimator;
  boompi::audio::CapturePlanes48k input{};
  constexpr std::size_t kTargetInputSample = 300U;
  constexpr std::size_t kTargetOutputSample = kTargetInputSample / 3U;

  for (std::size_t tap = 0U; tap < coefficients.size(); ++tap) {
    input[0][kTargetInputSample - tap] =
        coefficients[tap] < 0 ? std::numeric_limits<std::int16_t>::min()
                              : std::numeric_limits<std::int16_t>::max();
  }
  boompi::audio::CapturePlanes16k output{};
  auto result = decimator.Process(input, &output);
  BOOMPI_EXPECT(context, result.ok());
  BOOMPI_EXPECT(context, result.saturated_samples > 0U);
  BOOMPI_EXPECT(context,
                output[0][kTargetOutputSample] ==
                    std::numeric_limits<std::int16_t>::max());

  decimator.Reset();
  input = {};
  for (std::size_t tap = 0U; tap < coefficients.size(); ++tap) {
    input[0][kTargetInputSample - tap] =
        coefficients[tap] < 0 ? std::numeric_limits<std::int16_t>::max()
                              : std::numeric_limits<std::int16_t>::min();
  }
  result = decimator.Process(input, &output);
  BOOMPI_EXPECT(context, result.ok());
  BOOMPI_EXPECT(context, result.saturated_samples > 0U);
  BOOMPI_EXPECT(context,
                output[0][kTargetOutputSample] ==
                    std::numeric_limits<std::int16_t>::min());
}

void TestNullOutputDoesNotChangeState(boompi::test::TestContext& context) {
  boompi::audio::FirDecimator48To16 candidate;
  boompi::audio::FirDecimator48To16 control;
  boompi::audio::CapturePlanes48k prelude{};
  boompi::audio::CapturePlanes48k ignored{};
  boompi::audio::CapturePlanes48k probe{};
  for (std::size_t sample = 0U; sample < prelude[0].size(); ++sample) {
    for (std::size_t channel = 0U; channel < prelude.size(); ++channel) {
      prelude[channel][sample] = static_cast<std::int16_t>(
          static_cast<std::int32_t>((sample * 17U + channel * 101U) %
                                    16001U) -
          8000);
      ignored[channel][sample] = static_cast<std::int16_t>(12000);
      probe[channel][sample] = static_cast<std::int16_t>(
          static_cast<std::int32_t>((sample * 31U + channel * 53U) %
                                    14001U) -
          7000);
    }
  }

  boompi::audio::CapturePlanes16k scratch{};
  BOOMPI_EXPECT(context, candidate.Process(prelude, &scratch).ok());
  BOOMPI_EXPECT(context, control.Process(prelude, &scratch).ok());
  const auto invalid_result = candidate.Process(ignored, nullptr);
  BOOMPI_EXPECT(context, !invalid_result.ok());
  BOOMPI_EXPECT(context,
                invalid_result.code ==
                    boompi::audio::SampleTransformCode::kInvalidArgument);
  BOOMPI_EXPECT(context, invalid_result.saturated_samples == 0U);

  boompi::audio::CapturePlanes16k candidate_output{};
  boompi::audio::CapturePlanes16k control_output{};
  BOOMPI_EXPECT(context, candidate.Process(probe, &candidate_output).ok());
  BOOMPI_EXPECT(context, control.Process(probe, &control_output).ok());
  BOOMPI_EXPECT(context, PlanesEqual(candidate_output, control_output));
}

}  // namespace

int main() {
  boompi::test::TestContext context;
  TestZeroAndDc(context);
  TestCrossFrameImpulse(context);
  TestAgainstReferenceConvolution(context);
  TestFrequencyResponse(context);
  TestHalfLsbRoundingSymmetry(context);
  TestResetAndIdenticalChannels(context);
  TestSaturation(context);
  TestNullOutputDoesNotChangeState(context);

  if (context.failures() != 0U) {
    std::cerr << context.failures()
              << " FIR decimator expectation(s) failed\n";
    return 1;
  }
  std::cout << "boomPI FIR decimator tests passed\n";
  return 0;
}
