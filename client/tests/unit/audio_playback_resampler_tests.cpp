#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <type_traits>
#include <utility>

#include "boompi/audio/playback_resampler_24_to_48.h"
#include "boompi/test/test_context.h"

#define BOOMPI_EXPECT(context, expression) \
  (context).Expect((expression), #expression, __FILE__, __LINE__)

namespace {

constexpr double kPi = 3.141592653589793238462643383279502884;
constexpr std::int64_t kQ31Scale = INT64_C(2147483648);
using ReferenceCoefficients = std::array<
    std::int32_t, boompi::audio::PlaybackResampler24To48::kTapCount>;

static_assert(
    std::is_trivially_copyable<boompi::audio::PlaybackResampleResult>::value,
    "resampler result must remain a fixed hot-path value");
static_assert(
    noexcept(std::declval<boompi::audio::PlaybackResampler24To48&>().Process(
        std::declval<const boompi::audio::TtsPcmFrame24k&>(),
        std::declval<boompi::audio::ResampledPcmFrame48k*>())),
    "resampler Process must remain noexcept");
static_assert(
    noexcept(std::declval<boompi::audio::PlaybackResampler24To48&>().Drain(
        std::declval<boompi::audio::ResampledPcmFrame48k*>())),
    "resampler Drain must remain noexcept");
static_assert(boompi::audio::PlaybackResampler24To48::
                      kLatencyOutputSamples ==
                  32U,
              "P2f-a EOS limitation and timing tests assume 32 samples");
static_assert(boompi::audio::PlaybackResampler24To48::
                      kDrainSamples ==
                  63U,
              "65-tap exact-2N output must expose all drain positions");

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
      boompi::audio::PlaybackResampler24To48::kTapCount;
  constexpr std::size_t kMidpoint = (kTapCount - 1U) / 2U;
  constexpr double kKaiserBeta = 7.85726;
  std::array<double, kTapCount> floating{};
  const double window_denominator = BesselI0(kKaiserBeta);

  for (std::size_t tap = 0U; tap < kTapCount; ++tap) {
    const auto offset = static_cast<std::int32_t>(tap) -
                        static_cast<std::int32_t>(kMidpoint);
    double ideal = 1.0;
    if (offset != 0) {
      const double offset_value = static_cast<double>(offset);
      ideal = 2.0 * std::sin(kPi * offset_value / 2.0) /
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
    if (tap != kMidpoint && tap % 2U == 0U) {
      floating[tap] = 0.0;
    }
  }

  double outer_sum = 0.0;
  for (std::size_t tap = 0U; tap < kTapCount; ++tap) {
    if (tap != kMidpoint) {
      outer_sum += floating[tap];
    }
  }
  for (std::size_t tap = 0U; tap < kTapCount; ++tap) {
    if (tap != kMidpoint) {
      floating[tap] /= outer_sum;
    }
  }
  floating[kMidpoint] = 1.0;

  ReferenceCoefficients coefficients{};
  for (std::size_t tap = 0U; tap < kTapCount; ++tap) {
    coefficients[tap] = static_cast<std::int32_t>(
        std::llround(floating[tap] * static_cast<double>(kQ31Scale)));
  }
  coefficients[kMidpoint] = std::numeric_limits<std::int32_t>::max();

  std::int64_t sum = 0;
  for (const std::int32_t coefficient : coefficients) {
    sum += coefficient;
  }
  constexpr std::int64_t kTargetSum = 2 * kQ31Scale - 1;
  const std::int64_t symmetric_correction = (kTargetSum - sum) / 2;
  coefficients[kMidpoint - 1U] = static_cast<std::int32_t>(
      coefficients[kMidpoint - 1U] + symmetric_correction);
  coefficients[kMidpoint + 1U] = static_cast<std::int32_t>(
      coefficients[kMidpoint + 1U] + symmetric_correction);
  return coefficients;
}

std::int32_t RoundQ31Reference(const std::int64_t accumulator) {
  constexpr std::int64_t kHalf = kQ31Scale / 2;
  if (accumulator >= 0) {
    return static_cast<std::int32_t>((accumulator + kHalf) / kQ31Scale);
  }
  return static_cast<std::int32_t>(
      -((-accumulator + kHalf) / kQ31Scale));
}

boompi::audio::PlaybackGeneration MakeGeneration(
    const std::uint32_t epoch = 7U, const std::uint32_t turn_id = 11U,
    const std::uint32_t stream_id = 13U) {
  return boompi::audio::PlaybackGeneration{epoch, turn_id, stream_id};
}

boompi::audio::TtsPcmFrame24k MakeFrame(
    const boompi::audio::PlaybackGeneration& generation,
    const std::uint32_t sequence, const std::uint64_t timestamp_us,
    const std::uint16_t valid_samples =
        boompi::audio::TtsPcmFrame24k::kFrameSamples,
    const bool end_of_stream = false) {
  boompi::audio::TtsPcmFrame24k frame{};
  frame.format.sample_rate_hz =
      boompi::audio::TtsPcmFrame24k::kSampleRateHz;
  frame.format.frame_duration_ms =
      boompi::audio::TtsPcmFrame24k::kFrameDurationMs;
  frame.format.channels = boompi::audio::TtsPcmFrame24k::kChannelCount;
  frame.format.sample_format = boompi::audio::TtsPcmFrame24k::kSampleFormat;
  frame.metadata.monotonic_timestamp_us = timestamp_us;
  frame.metadata.sequence = sequence;
  frame.metadata.epoch = generation.epoch;
  frame.metadata.turn_id = generation.turn_id;
  frame.metadata.stream_id = generation.stream_id;
  frame.valid_source_samples = valid_samples;
  frame.end_of_stream = end_of_stream;
  return frame;
}

bool MetadataEqual(const boompi::audio::AudioFrameMetadata& lhs,
                   const boompi::audio::AudioFrameMetadata& rhs) {
  return lhs.monotonic_timestamp_us == rhs.monotonic_timestamp_us &&
         lhs.sequence == rhs.sequence && lhs.stream_id == rhs.stream_id &&
         lhs.turn_id == rhs.turn_id && lhs.epoch == rhs.epoch &&
         lhs.discontinuity == rhs.discontinuity;
}

bool AllSamplesEqual(const boompi::audio::ResampledPcmFrame48k& frame,
                     const std::int32_t expected) {
  for (std::size_t sample = 0U; sample < frame.valid_samples; ++sample) {
    if (frame.samples[sample] != expected) {
      return false;
    }
  }
  return true;
}

bool HeaderIsInvalid(const boompi::audio::ResampledPcmFrame48k& frame) {
  return !frame.HasValidLength() && frame.valid_samples == 0U &&
         frame.metadata.epoch == 0U && frame.metadata.stream_id == 0U &&
         frame.metadata.turn_id == 0U;
}

void TestDcImpulseAndMetadata(boompi::test::TestContext& context) {
  const auto generation = MakeGeneration();
  boompi::audio::PlaybackResampler24To48 resampler;
  BOOMPI_EXPECT(context, resampler.Arm(generation).ok());

  auto first = MakeFrame(generation, 4U, 1000000U);
  first.samples.fill(12345);
  boompi::audio::ResampledPcmFrame48k output{};
  auto result = resampler.Process(first, &output);
  BOOMPI_EXPECT(context, result.produced());
  BOOMPI_EXPECT(context, !result.drain_required);

  auto second = MakeFrame(generation, 5U, 1020000U);
  second.samples.fill(12345);
  result = resampler.Process(second, &output);
  BOOMPI_EXPECT(context, result.produced());
  BOOMPI_EXPECT(context, output.HasValidLength());
  BOOMPI_EXPECT(context, AllSamplesEqual(output, 12345));
  BOOMPI_EXPECT(context,
                output.valid_samples ==
                    boompi::audio::ResampledPcmFrame48k::kFrameSamples);
  BOOMPI_EXPECT(context, output.source_offset_sample_frames == 0U);
  BOOMPI_EXPECT(context, !output.end_of_stream);
  BOOMPI_EXPECT(context, MetadataEqual(output.metadata, second.metadata));

  const auto impulse_generation = MakeGeneration(8U, 12U, 14U);
  BOOMPI_EXPECT(context, !resampler.Arm(impulse_generation).ok());
  resampler.Disarm();
  BOOMPI_EXPECT(context, resampler.Arm(impulse_generation).ok());
  auto impulse = MakeFrame(impulse_generation, 0U, 2000000U);
  constexpr std::size_t kImpulseIndex = 100U;
  constexpr std::int16_t kImpulseAmplitude = 32767;
  impulse.samples[kImpulseIndex] = kImpulseAmplitude;
  result = resampler.Process(impulse, &output);
  BOOMPI_EXPECT(context, result.produced());

  const auto coefficients = GenerateReferenceCoefficients();
  bool impulse_matches = true;
  for (std::size_t tap = 0U; tap < coefficients.size(); ++tap) {
    const auto expected = RoundQ31Reference(
        static_cast<std::int64_t>(coefficients[tap]) *
        kImpulseAmplitude);
    impulse_matches =
        impulse_matches &&
        output.samples[kImpulseIndex * 2U + tap] == expected;
  }
  BOOMPI_EXPECT(context, impulse_matches);
  BOOMPI_EXPECT(
      context,
      output.samples[kImpulseIndex * 2U +
                     boompi::audio::PlaybackResampler24To48::
                         kLatencyOutputSamples] == kImpulseAmplitude);
}

void TestCrossFrameAndReset(boompi::test::TestContext& context) {
  const auto coefficients = GenerateReferenceCoefficients();
  const auto generation = MakeGeneration(20U, 21U, 22U);
  boompi::audio::PlaybackResampler24To48 resampler;
  BOOMPI_EXPECT(context, resampler.Arm(generation).ok());

  auto first = MakeFrame(generation, 0U, 3000000U);
  constexpr std::size_t kImpulseIndex =
      boompi::audio::TtsPcmFrame24k::kFrameSamples - 1U;
  constexpr std::int16_t kImpulseAmplitude = 24000;
  first.samples[kImpulseIndex] = kImpulseAmplitude;
  auto second = MakeFrame(generation, 1U, 3020000U);
  std::array<boompi::audio::ResampledPcmFrame48k, 2U> output{};
  BOOMPI_EXPECT(context, resampler.Process(first, &output[0]).produced());
  BOOMPI_EXPECT(context, resampler.Process(second, &output[1]).produced());

  bool matches = true;
  for (std::size_t output_sample = 0U;
       output_sample < boompi::audio::ResampledPcmFrame48k::kFrameSamples *
                           2U;
       ++output_sample) {
    std::int32_t expected = 0;
    const std::size_t impulse_output_index = kImpulseIndex * 2U;
    if (output_sample >= impulse_output_index &&
        output_sample - impulse_output_index < coefficients.size()) {
      expected = RoundQ31Reference(
          static_cast<std::int64_t>(
              coefficients[output_sample - impulse_output_index]) *
          kImpulseAmplitude);
    }
    const std::size_t frame_index =
        output_sample /
        boompi::audio::ResampledPcmFrame48k::kFrameSamples;
    const std::size_t frame_sample =
        output_sample %
        boompi::audio::ResampledPcmFrame48k::kFrameSamples;
    matches = matches && output[frame_index].samples[frame_sample] == expected;
  }
  BOOMPI_EXPECT(context, matches);

  resampler.Disarm();
  BOOMPI_EXPECT(context, !resampler.Arm(generation).ok());
  const auto reset_generation = MakeGeneration(23U, 24U, 25U);
  BOOMPI_EXPECT(context, resampler.Arm(reset_generation).ok());
  const auto zeros = MakeFrame(reset_generation, 0U, 4000000U);
  boompi::audio::ResampledPcmFrame48k reset_output{};
  BOOMPI_EXPECT(context, resampler.Process(zeros, &reset_output).produced());
  BOOMPI_EXPECT(context, AllSamplesEqual(reset_output, 0));
}

void TestEveryEosLengthAndDrain(boompi::test::TestContext& context) {
  const auto coefficients = GenerateReferenceCoefficients();
  boompi::audio::PlaybackResampler24To48 resampler;

  for (std::size_t valid_source_samples = 1U;
       valid_source_samples <= boompi::audio::TtsPcmFrame24k::kFrameSamples;
       ++valid_source_samples) {
    const auto generation = MakeGeneration(
        static_cast<std::uint32_t>(1000U + valid_source_samples),
        static_cast<std::uint32_t>(2000U + valid_source_samples),
        static_cast<std::uint32_t>(3000U + valid_source_samples));
    BOOMPI_EXPECT(context, resampler.Arm(generation).ok());
    auto eos = MakeFrame(
        generation, 0U, 5000000U + valid_source_samples * 20000U,
        static_cast<std::uint16_t>(valid_source_samples), true);
    for (std::size_t sample = 0U; sample < valid_source_samples; ++sample) {
      eos.samples[sample] = static_cast<std::int16_t>(
          static_cast<std::int32_t>(
              (sample * 251U + valid_source_samples * 17U) % 24001U) -
          12000);
    }

    boompi::audio::ResampledPcmFrame48k prefix{};
    const auto prefix_result = resampler.Process(eos, &prefix);
    BOOMPI_EXPECT(context, prefix_result.produced());
    BOOMPI_EXPECT(context, prefix_result.drain_required);
    BOOMPI_EXPECT(context, prefix.HasValidLength());
    BOOMPI_EXPECT(context, !prefix.end_of_stream);
    BOOMPI_EXPECT(context, prefix.source_offset_sample_frames == 0U);
    BOOMPI_EXPECT(context,
                  prefix.valid_samples == valid_source_samples * 2U);
    BOOMPI_EXPECT(context, MetadataEqual(prefix.metadata, eos.metadata));

    boompi::audio::ResampledPcmFrame48k invalid_output{};
    if (valid_source_samples == 1U) {
      const auto null_process = resampler.Process(eos, nullptr);
      BOOMPI_EXPECT(
          context,
          null_process.code ==
              boompi::audio::PlaybackResampleCode::kInvalidArgument);
      BOOMPI_EXPECT(context, null_process.drain_required);

      auto stale = eos;
      stale.metadata.epoch += 1U;
      const auto stale_result = resampler.Process(stale, &invalid_output);
      BOOMPI_EXPECT(
          context,
          stale_result.code ==
              boompi::audio::PlaybackResampleCode::kEpochMismatch);
      BOOMPI_EXPECT(context, stale_result.drain_required);
      BOOMPI_EXPECT(context, HeaderIsInvalid(invalid_output));
    }
    const auto blocked = resampler.Process(eos, &invalid_output);
    BOOMPI_EXPECT(context,
                  blocked.code ==
                      boompi::audio::PlaybackResampleCode::kDrainPending);
    BOOMPI_EXPECT(context, blocked.drain_required);
    BOOMPI_EXPECT(context, HeaderIsInvalid(invalid_output));
    const auto null_drain = resampler.Drain(nullptr);
    BOOMPI_EXPECT(context,
                  null_drain.code ==
                      boompi::audio::PlaybackResampleCode::kInvalidArgument);
    BOOMPI_EXPECT(context, null_drain.drain_required);

    boompi::audio::ResampledPcmFrame48k drain{};
    const auto drain_result = resampler.Drain(&drain);
    BOOMPI_EXPECT(context, drain_result.produced());
    BOOMPI_EXPECT(context, !drain_result.drain_required);
    BOOMPI_EXPECT(context, drain.HasValidLength());
    BOOMPI_EXPECT(context, drain.end_of_stream);
    BOOMPI_EXPECT(
        context,
        drain.source_offset_sample_frames == valid_source_samples * 2U);
    BOOMPI_EXPECT(
        context,
        drain.valid_samples ==
            boompi::audio::PlaybackResampler24To48::kDrainSamples);
    BOOMPI_EXPECT(context, MetadataEqual(drain.metadata, eos.metadata));

    bool convolution_matches = true;
    const std::size_t prefix_samples = valid_source_samples * 2U;
    const std::size_t convolution_samples =
        prefix_samples +
        boompi::audio::PlaybackResampler24To48::kDrainSamples;
    for (std::size_t output_sample = 0U;
         output_sample < convolution_samples; ++output_sample) {
      std::int64_t accumulator = 0;
      for (std::size_t tap = 0U; tap < coefficients.size(); ++tap) {
        if (output_sample < tap || (output_sample - tap) % 2U != 0U) {
          continue;
        }
        const std::size_t source_sample = (output_sample - tap) / 2U;
        if (source_sample < valid_source_samples) {
          accumulator +=
              static_cast<std::int64_t>(coefficients[tap]) *
              eos.samples[source_sample];
        }
      }
      const std::int32_t expected = RoundQ31Reference(accumulator);
      const std::int32_t actual =
          output_sample < prefix_samples
              ? prefix.samples[output_sample]
              : drain.samples[output_sample - prefix_samples];
      convolution_matches = convolution_matches && actual == expected;
    }
    BOOMPI_EXPECT(context, convolution_matches);
    // The 63rd drain position is an explicit termination zero because h[64]
    // is exactly zero; the potentially non-zero convolution tail is 62 long.
    BOOMPI_EXPECT(
        context,
        drain.samples[boompi::audio::PlaybackResampler24To48::kDrainSamples -
                      1U] == 0);
    bool unused_tail_is_zero = true;
    for (std::size_t sample = drain.valid_samples;
         sample < drain.samples.size(); ++sample) {
      unused_tail_is_zero = unused_tail_is_zero && drain.samples[sample] == 0;
    }
    BOOMPI_EXPECT(context, unused_tail_is_zero);
    BOOMPI_EXPECT(context,
                  resampler.Drain(&invalid_output).code ==
                      boompi::audio::PlaybackResampleCode::kNotArmed);
    BOOMPI_EXPECT(context, !resampler.Arm(generation).ok());
  }
}

struct ToneMeasurement final {
  double gain{0.0};
  double image_to_signal{0.0};
};

ToneMeasurement MeasureTone(const double frequency_hz) {
  constexpr std::size_t kFrameCount = 12U;
  constexpr std::size_t kWarmupFrames = 2U;
  constexpr double kAmplitude = 16000.0;
  constexpr double kPhase = 0.371;
  const auto generation = MakeGeneration(40U, 41U, 42U);
  boompi::audio::PlaybackResampler24To48 resampler;
  if (!resampler.Arm(generation).ok()) {
    return {};
  }

  double input_energy = 0.0;
  double output_energy = 0.0;
  double desired_real = 0.0;
  double desired_imag = 0.0;
  double image_real = 0.0;
  double image_imag = 0.0;
  std::size_t input_count = 0U;
  std::size_t output_count = 0U;
  const double image_frequency_hz = 24000.0 - frequency_hz;

  for (std::size_t frame_index = 0U; frame_index < kFrameCount;
       ++frame_index) {
    auto frame = MakeFrame(
        generation, static_cast<std::uint32_t>(frame_index),
        7000000U + static_cast<std::uint64_t>(frame_index) * 20000U);
    for (std::size_t sample = 0U; sample < frame.samples.size(); ++sample) {
      const std::size_t global_source_sample =
          frame_index * frame.samples.size() + sample;
      const auto value = static_cast<std::int16_t>(std::lround(
          kAmplitude *
          std::sin(2.0 * kPi * frequency_hz *
                       static_cast<double>(global_source_sample) / 24000.0 +
                   kPhase)));
      frame.samples[sample] = value;
      if (frame_index >= kWarmupFrames) {
        input_energy += static_cast<double>(value) * value;
        ++input_count;
      }
    }

    boompi::audio::ResampledPcmFrame48k output{};
    const auto result = resampler.Process(frame, &output);
    if (!result.produced() || result.drain_required) {
      return {};
    }
    if (frame_index < kWarmupFrames) {
      continue;
    }
    for (std::size_t sample = 0U; sample < output.valid_samples; ++sample) {
      const std::size_t global_output_sample =
          frame_index * output.samples.size() + sample;
      const double value = output.samples[sample];
      output_energy += value * value;
      const double desired_phase =
          2.0 * kPi * frequency_hz *
          static_cast<double>(global_output_sample) / 48000.0;
      const double image_phase =
          2.0 * kPi * image_frequency_hz *
          static_cast<double>(global_output_sample) / 48000.0;
      desired_real += value * std::cos(desired_phase);
      desired_imag -= value * std::sin(desired_phase);
      image_real += value * std::cos(image_phase);
      image_imag -= value * std::sin(image_phase);
      ++output_count;
    }
  }

  const double input_rms =
      std::sqrt(input_energy / static_cast<double>(input_count));
  const double output_rms =
      std::sqrt(output_energy / static_cast<double>(output_count));
  const double desired_magnitude =
      std::sqrt(desired_real * desired_real + desired_imag * desired_imag);
  const double image_magnitude =
      std::sqrt(image_real * image_real + image_imag * image_imag);
  return {output_rms / input_rms, image_magnitude / desired_magnitude};
}

void TestFrequencyResponse(boompi::test::TestContext& context) {
  for (const double frequency_hz : {1000.0, 6000.0, 10000.0}) {
    const ToneMeasurement measurement = MeasureTone(frequency_hz);
    const double gain_db = 20.0 * std::log10(measurement.gain);
    BOOMPI_EXPECT(context, std::abs(gain_db) <= 0.01);
  }
  // The closest image is 14 kHz for a 10 kHz source. Allow quantized PCM
  // measurement noise while preserving more than 70 dB image rejection.
  BOOMPI_EXPECT(context, MeasureTone(10000.0).image_to_signal <= 0.00031623);
}

void TestWideOvershoot(boompi::test::TestContext& context) {
  const auto coefficients = GenerateReferenceCoefficients();
  constexpr std::size_t kTargetSourceSample = 100U;
  const auto generation = MakeGeneration(50U, 51U, 52U);
  boompi::audio::PlaybackResampler24To48 positive;
  BOOMPI_EXPECT(context, positive.Arm(generation).ok());
  auto frame = MakeFrame(generation, 0U, 8000000U);
  for (std::size_t phase_tap = 0U;
       phase_tap < boompi::audio::PlaybackResampler24To48::kOddPhaseTapCount;
       ++phase_tap) {
    const std::int32_t coefficient = coefficients[phase_tap * 2U + 1U];
    frame.samples[kTargetSourceSample - phase_tap] =
        coefficient < 0 ? std::numeric_limits<std::int16_t>::min()
                        : std::numeric_limits<std::int16_t>::max();
  }
  boompi::audio::ResampledPcmFrame48k output{};
  auto result = positive.Process(frame, &output);
  BOOMPI_EXPECT(context, result.produced());
  std::int64_t positive_accumulator = 0;
  for (std::size_t phase_tap = 0U;
       phase_tap < boompi::audio::PlaybackResampler24To48::kOddPhaseTapCount;
       ++phase_tap) {
    positive_accumulator +=
        static_cast<std::int64_t>(coefficients[phase_tap * 2U + 1U]) *
        frame.samples[kTargetSourceSample - phase_tap];
  }
  const std::int32_t positive_expected =
      RoundQ31Reference(positive_accumulator);
  BOOMPI_EXPECT(context,
                output.samples[kTargetSourceSample * 2U + 1U] ==
                    positive_expected);
  BOOMPI_EXPECT(context,
                positive_expected >
                    std::numeric_limits<std::int16_t>::max());

  const auto negative_generation = MakeGeneration(53U, 54U, 55U);
  boompi::audio::PlaybackResampler24To48 negative;
  BOOMPI_EXPECT(context, negative.Arm(negative_generation).ok());
  frame = MakeFrame(negative_generation, 0U, 9000000U);
  for (std::size_t phase_tap = 0U;
       phase_tap < boompi::audio::PlaybackResampler24To48::kOddPhaseTapCount;
       ++phase_tap) {
    const std::int32_t coefficient = coefficients[phase_tap * 2U + 1U];
    frame.samples[kTargetSourceSample - phase_tap] =
        coefficient < 0 ? std::numeric_limits<std::int16_t>::max()
                        : std::numeric_limits<std::int16_t>::min();
  }
  result = negative.Process(frame, &output);
  BOOMPI_EXPECT(context, result.produced());
  std::int64_t negative_accumulator = 0;
  for (std::size_t phase_tap = 0U;
       phase_tap < boompi::audio::PlaybackResampler24To48::kOddPhaseTapCount;
       ++phase_tap) {
    negative_accumulator +=
        static_cast<std::int64_t>(coefficients[phase_tap * 2U + 1U]) *
        frame.samples[kTargetSourceSample - phase_tap];
  }
  const std::int32_t negative_expected =
      RoundQ31Reference(negative_accumulator);
  BOOMPI_EXPECT(context,
                output.samples[kTargetSourceSample * 2U + 1U] ==
                    negative_expected);
  BOOMPI_EXPECT(context,
                negative_expected <
                    std::numeric_limits<std::int16_t>::min());
}

void TestGenerationAndErrorPaths(boompi::test::TestContext& context) {
  boompi::audio::PlaybackResampler24To48 resampler;
  boompi::audio::ResampledPcmFrame48k output{};
  output.valid_samples = 100U;
  const auto generation = MakeGeneration(60U, 61U, 62U);
  auto frame = MakeFrame(generation, 0U, 10000000U);

  BOOMPI_EXPECT(context,
                resampler.Process(frame, &output).code ==
                    boompi::audio::PlaybackResampleCode::kNotArmed);
  BOOMPI_EXPECT(context, HeaderIsInvalid(output));
  BOOMPI_EXPECT(context, !resampler.Arm({0U, 1U, 1U}).ok());
  BOOMPI_EXPECT(context, resampler.Arm(generation).ok());
  BOOMPI_EXPECT(context,
                resampler.Drain(&output).code ==
                    boompi::audio::PlaybackResampleCode::kNoDrainPending);
  BOOMPI_EXPECT(context, HeaderIsInvalid(output));

  auto wrong = frame;
  wrong.metadata.epoch += 1U;
  BOOMPI_EXPECT(context,
                resampler.Process(wrong, &output).code ==
                    boompi::audio::PlaybackResampleCode::kEpochMismatch);
  BOOMPI_EXPECT(context, HeaderIsInvalid(output));
  wrong = frame;
  wrong.metadata.stream_id += 1U;
  BOOMPI_EXPECT(context,
                resampler.Process(wrong, &output).code ==
                    boompi::audio::PlaybackResampleCode::kStreamMismatch);
  wrong = frame;
  wrong.metadata.turn_id += 1U;
  BOOMPI_EXPECT(context,
                resampler.Process(wrong, &output).code ==
                    boompi::audio::PlaybackResampleCode::kTurnMismatch);
  BOOMPI_EXPECT(context, resampler.Process(frame, &output).produced());

  auto second = MakeFrame(generation, 1U, 10020000U);
  BOOMPI_EXPECT(context,
                resampler.Process(second, nullptr).code ==
                    boompi::audio::PlaybackResampleCode::kInvalidArgument);
  BOOMPI_EXPECT(context, resampler.Process(second, &output).produced());

  auto malformed = MakeFrame(generation, 2U, 10040000U, 37U, false);
  BOOMPI_EXPECT(context,
                resampler.Process(malformed, &output).code ==
                    boompi::audio::PlaybackResampleCode::kInvalidInputFrame);
  BOOMPI_EXPECT(context, HeaderIsInvalid(output));
  const auto valid_after_fault = MakeFrame(generation, 2U, 10040000U);
  BOOMPI_EXPECT(context,
                resampler.Process(valid_after_fault, &output).code ==
                    boompi::audio::PlaybackResampleCode::kFaulted);
  BOOMPI_EXPECT(context,
                resampler.Drain(&output).code ==
                    boompi::audio::PlaybackResampleCode::kFaulted);
  BOOMPI_EXPECT(context, HeaderIsInvalid(output));

  const auto discontinuity_generation = MakeGeneration(63U, 64U, 65U);
  resampler.Disarm();
  BOOMPI_EXPECT(context, resampler.Arm(discontinuity_generation).ok());
  auto discontinuity =
      MakeFrame(discontinuity_generation, 0U, 11000000U);
  discontinuity.metadata.discontinuity = true;
  BOOMPI_EXPECT(context,
                resampler.Process(discontinuity, &output).code ==
                    boompi::audio::PlaybackResampleCode::kDiscontinuity);
  discontinuity.metadata.discontinuity = false;
  BOOMPI_EXPECT(context,
                resampler.Process(discontinuity, &output).code ==
                    boompi::audio::PlaybackResampleCode::kFaulted);

  const auto sequence_generation = MakeGeneration(66U, 67U, 68U);
  resampler.Disarm();
  BOOMPI_EXPECT(context, resampler.Arm(sequence_generation).ok());
  BOOMPI_EXPECT(context,
                resampler
                    .Process(MakeFrame(sequence_generation, 0U, 12000000U),
                             &output)
                    .produced());
  BOOMPI_EXPECT(context,
                resampler
                        .Process(
                            MakeFrame(sequence_generation, 2U, 12020000U),
                            &output)
                        .code ==
                    boompi::audio::PlaybackResampleCode::kSequenceBreak);
  BOOMPI_EXPECT(context,
                resampler
                        .Process(
                            MakeFrame(sequence_generation, 1U, 12020000U),
                            &output)
                        .code == boompi::audio::PlaybackResampleCode::kFaulted);

  const auto timestamp_generation = MakeGeneration(69U, 70U, 71U);
  resampler.Disarm();
  BOOMPI_EXPECT(context, resampler.Arm(timestamp_generation).ok());
  BOOMPI_EXPECT(context,
                resampler
                    .Process(MakeFrame(timestamp_generation, 0U, 13000000U),
                             &output)
                    .produced());
  BOOMPI_EXPECT(
      context,
      resampler
              .Process(MakeFrame(timestamp_generation, 1U, 13000000U),
                       &output)
              .code ==
          boompi::audio::PlaybackResampleCode::kTimestampNotIncreasing);
}

}  // namespace

int main() {
  boompi::test::TestContext context;
  TestDcImpulseAndMetadata(context);
  TestCrossFrameAndReset(context);
  TestEveryEosLengthAndDrain(context);
  TestFrequencyResponse(context);
  TestWideOvershoot(context);
  TestGenerationAndErrorPaths(context);

  if (context.failures() != 0U) {
    std::cerr << context.failures()
              << " playback resampler expectation(s) failed\n";
    return 1;
  }
  std::cout << "boomPI playback resampler tests passed\n";
  return 0;
}
