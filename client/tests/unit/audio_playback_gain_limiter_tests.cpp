#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <type_traits>
#include <utility>

#include "boompi/audio/playback_gain_limiter.h"
#include "boompi/test/test_context.h"

#define BOOMPI_EXPECT(context, expression) \
  (context).Expect((expression), #expression, __FILE__, __LINE__)

namespace {

using boompi::StatusCode;
using boompi::audio::PlaybackGainLimiter;
using boompi::audio::PlaybackGainLimiterCode;
using boompi::audio::PlaybackGainLimiterConfig;
using boompi::audio::PlaybackGainLimiterResult;
using boompi::audio::PlaybackGeneration;
using boompi::audio::PlaybackPcmFrame48k;
using boompi::audio::ResampledPcmFrame48k;
using boompi::audio::SampleFormat;

static_assert(std::is_trivial<PlaybackGainLimiterResult>::value,
              "gain/limiter result must remain trivial");
static_assert(std::is_standard_layout<PlaybackGainLimiterResult>::value,
              "gain/limiter result must remain standard-layout");
static_assert(
    noexcept(std::declval<PlaybackGainLimiter&>().Process(
        std::declval<const ResampledPcmFrame48k&>(),
        std::declval<PlaybackPcmFrame48k*>())),
    "gain/limiter hot path must remain noexcept");

PlaybackGainLimiterConfig UnityConfig() {
  PlaybackGainLimiterConfig config{};
  config.volume_percent = 100U;
  config.speaker_gain_percent = 100U;
  config.duck_target_percent = 25U;
  config.duck_ramp_ms = 80U;
  config.recovery_ramp_ms = 80U;
  config.limiter_ceiling_percent = 100U;
  config.limiter_release_ms = 80U;
  return config;
}

PlaybackGeneration Generation(const std::uint32_t epoch,
                              const std::uint32_t stream_id,
                              const std::uint32_t turn_id = 42U) {
  return {epoch, turn_id, stream_id};
}

ResampledPcmFrame48k MakeFrame(
    const std::uint32_t epoch, const std::uint32_t stream_id,
    const std::int32_t sample_value = 10000,
    const std::uint16_t valid_samples =
        ResampledPcmFrame48k::kFrameSamples,
    const bool end_of_stream = false,
    const std::uint32_t turn_id = 42U,
    const std::uint16_t source_offset_sample_frames = 0U) {
  ResampledPcmFrame48k frame{};
  frame.metadata.monotonic_timestamp_us = 1000U;
  frame.metadata.sequence = 7U;
  frame.metadata.stream_id = stream_id;
  frame.metadata.turn_id = turn_id;
  frame.metadata.epoch = epoch;
  frame.source_offset_sample_frames = source_offset_sample_frames;
  frame.valid_samples = valid_samples;
  frame.end_of_stream = end_of_stream;
  for (std::size_t sample = 0U; sample < frame.samples.size(); ++sample) {
    frame.samples[sample] =
        sample < valid_samples ? sample_value : static_cast<std::int32_t>(0);
  }
  return frame;
}

void FillOutputSentinel(PlaybackPcmFrame48k* const output,
                        const std::int16_t pcm_sentinel) {
  *output = PlaybackPcmFrame48k{};
  output->format = {48000U, 20U, 1U, SampleFormat::kPcmS16Le};
  output->metadata.monotonic_timestamp_us = 1000U;
  output->metadata.sequence = 7U;
  output->metadata.stream_id = 92U;
  output->metadata.turn_id = 42U;
  output->metadata.epoch = 91U;
  output->valid_samples = PlaybackPcmFrame48k::kFrameSamples;
  output->samples.fill(pcm_sentinel);
  output->metadata.discontinuity = true;
}

bool MetadataEqual(const boompi::audio::AudioFrameMetadata& lhs,
                   const boompi::audio::AudioFrameMetadata& rhs) {
  return lhs.monotonic_timestamp_us == rhs.monotonic_timestamp_us &&
         lhs.sequence == rhs.sequence && lhs.stream_id == rhs.stream_id &&
         lhs.turn_id == rhs.turn_id && lhs.epoch == rhs.epoch &&
         lhs.discontinuity == rhs.discontinuity;
}

void ExpectInvalidHeaderPreservingPcm(
    boompi::test::TestContext& context,
    const PlaybackPcmFrame48k& output,
    const std::int16_t pcm_sentinel) {
  BOOMPI_EXPECT(context, !output.HasValidLength());
  BOOMPI_EXPECT(context, output.format.sample_rate_hz == 0U);
  BOOMPI_EXPECT(context, output.format.frame_duration_ms == 0U);
  BOOMPI_EXPECT(context, output.format.channels == 0U);
  BOOMPI_EXPECT(context, output.metadata.monotonic_timestamp_us == 0U);
  BOOMPI_EXPECT(context, output.metadata.sequence == 0U);
  BOOMPI_EXPECT(context, output.metadata.stream_id == 0U);
  BOOMPI_EXPECT(context, output.metadata.turn_id == 0U);
  BOOMPI_EXPECT(context, output.metadata.epoch == 0U);
  BOOMPI_EXPECT(context, !output.metadata.discontinuity);
  BOOMPI_EXPECT(context, output.valid_samples == 0U);
  BOOMPI_EXPECT(context, output.source_offset_sample_frames == 0U);
  BOOMPI_EXPECT(context, !output.end_of_stream);
  BOOMPI_EXPECT(context, output.samples.front() == pcm_sentinel);
  BOOMPI_EXPECT(context, output.samples.back() == pcm_sentinel);
}

bool AllValidSamplesEqual(const PlaybackPcmFrame48k& frame,
                          const std::int16_t expected) {
  for (std::size_t sample = 0U; sample < frame.valid_samples; ++sample) {
    if (frame.samples[sample] != expected) {
      return false;
    }
  }
  return true;
}

void TestConfigurationValidation(boompi::test::TestContext& context) {
  const PlaybackGainLimiterConfig defaults{};
  BOOMPI_EXPECT(context, defaults.volume_percent == 60U);
  BOOMPI_EXPECT(context, defaults.speaker_gain_percent == 100U);
  BOOMPI_EXPECT(context, defaults.duck_ramp_ms == 80U);
  BOOMPI_EXPECT(context, defaults.recovery_ramp_ms == 80U);
  BOOMPI_EXPECT(context, defaults.limiter_release_ms == 80U);

  auto status = PlaybackGainLimiter::Create(defaults, nullptr);
  BOOMPI_EXPECT(context, !status.ok());
  BOOMPI_EXPECT(context, status.code() == StatusCode::kInvalidArgument);

  PlaybackGainLimiter limiter;
  status = limiter.Arm(Generation(1U, 1U));
  BOOMPI_EXPECT(context, !status.ok());
  BOOMPI_EXPECT(context, status.code() == StatusCode::kFailedPrecondition);

  auto invalid = defaults;
  invalid.volume_percent = 101U;
  status = PlaybackGainLimiter::Create(invalid, &limiter);
  BOOMPI_EXPECT(context, !status.ok());
  BOOMPI_EXPECT(context, status.code() == StatusCode::kInvalidArgument);
  invalid = defaults;
  invalid.speaker_gain_percent = 401U;
  status = PlaybackGainLimiter::Create(invalid, &limiter);
  BOOMPI_EXPECT(context, !status.ok());
  BOOMPI_EXPECT(context, status.code() == StatusCode::kInvalidArgument);
  invalid = defaults;
  invalid.duck_target_percent = 101U;
  status = PlaybackGainLimiter::Create(invalid, &limiter);
  BOOMPI_EXPECT(context, !status.ok());
  BOOMPI_EXPECT(context, status.code() == StatusCode::kInvalidArgument);
  invalid = defaults;
  invalid.duck_ramp_ms = 1001U;
  status = PlaybackGainLimiter::Create(invalid, &limiter);
  BOOMPI_EXPECT(context, !status.ok());
  BOOMPI_EXPECT(context, status.code() == StatusCode::kInvalidArgument);
  invalid = defaults;
  invalid.recovery_ramp_ms = 1001U;
  status = PlaybackGainLimiter::Create(invalid, &limiter);
  BOOMPI_EXPECT(context, !status.ok());
  BOOMPI_EXPECT(context, status.code() == StatusCode::kInvalidArgument);
  invalid = defaults;
  invalid.limiter_ceiling_percent = 0U;
  status = PlaybackGainLimiter::Create(invalid, &limiter);
  BOOMPI_EXPECT(context, !status.ok());
  BOOMPI_EXPECT(context, status.code() == StatusCode::kInvalidArgument);
  invalid.limiter_ceiling_percent = 101U;
  status = PlaybackGainLimiter::Create(invalid, &limiter);
  BOOMPI_EXPECT(context, !status.ok());
  BOOMPI_EXPECT(context, status.code() == StatusCode::kInvalidArgument);
  invalid = defaults;
  invalid.limiter_release_ms = 1001U;
  status = PlaybackGainLimiter::Create(invalid, &limiter);
  BOOMPI_EXPECT(context, !status.ok());
  BOOMPI_EXPECT(context, status.code() == StatusCode::kInvalidArgument);

  status = PlaybackGainLimiter::Create(defaults, &limiter);
  BOOMPI_EXPECT(context, status.ok());
  status = PlaybackGainLimiter::Create(defaults, &limiter);
  BOOMPI_EXPECT(context, !status.ok());
  BOOMPI_EXPECT(context, status.code() == StatusCode::kFailedPrecondition);
  status = limiter.SetDucked(true);
  BOOMPI_EXPECT(context, !status.ok());
  BOOMPI_EXPECT(context, status.code() == StatusCode::kFailedPrecondition);
  status = limiter.Arm(Generation(0U, 1U));
  BOOMPI_EXPECT(context, !status.ok());
  BOOMPI_EXPECT(context, status.code() == StatusCode::kInvalidArgument);
  status = limiter.Arm(Generation(1U, 0U));
  BOOMPI_EXPECT(context, !status.ok());
  BOOMPI_EXPECT(context, status.code() == StatusCode::kInvalidArgument);
  status = limiter.Arm(Generation(1U, 1U, 0U));
  BOOMPI_EXPECT(context, !status.ok());
  BOOMPI_EXPECT(context, status.code() == StatusCode::kInvalidArgument);

  constexpr std::int16_t kSentinel = 12345;
  PlaybackPcmFrame48k output{};
  FillOutputSentinel(&output, kSentinel);
  const auto result = limiter.Process(MakeFrame(1U, 1U), &output);
  BOOMPI_EXPECT(context, result.code == PlaybackGainLimiterCode::kNotArmed);
  BOOMPI_EXPECT(context, !result.produced());
  BOOMPI_EXPECT(context, !result.requires_new_generation());
  ExpectInvalidHeaderPreservingPcm(context, output, kSentinel);
}

void TestUnityMuteAndMetadata(boompi::test::TestContext& context) {
  PlaybackGainLimiter unity;
  BOOMPI_EXPECT(
      context,
      PlaybackGainLimiter::Create(UnityConfig(), &unity).ok());
  BOOMPI_EXPECT(context, unity.Arm(Generation(1U, 2U)).ok());

  auto input = MakeFrame(1U, 2U, 0);
  for (std::size_t sample = 0U; sample < input.samples.size(); ++sample) {
    input.samples[sample] = static_cast<std::int16_t>(
        static_cast<std::int32_t>((sample * 67U) % 65536U) - 32768);
  }
  input.samples[0U] = std::numeric_limits<std::int16_t>::min();
  input.samples[1U] = std::numeric_limits<std::int16_t>::max();
  PlaybackPcmFrame48k output{};
  auto result = unity.Process(input, &output);
  BOOMPI_EXPECT(context, result.code == PlaybackGainLimiterCode::kProduced);
  BOOMPI_EXPECT(context, result.produced());
  BOOMPI_EXPECT(context, result.over_full_scale_samples == 0U);
  BOOMPI_EXPECT(context, result.limited_samples == 0U);
  BOOMPI_EXPECT(context, result.safety_saturated_samples == 0U);
  bool samples_match = true;
  for (std::size_t sample = 0U; sample < output.samples.size(); ++sample) {
    samples_match = samples_match && output.samples[sample] == input.samples[sample];
  }
  BOOMPI_EXPECT(context, samples_match);
  BOOMPI_EXPECT(context, output.format.sample_rate_hz == 48000U);
  BOOMPI_EXPECT(context, MetadataEqual(output.metadata, input.metadata));
  BOOMPI_EXPECT(context, output.valid_samples == input.valid_samples);
  BOOMPI_EXPECT(context,
                output.source_offset_sample_frames ==
                    input.source_offset_sample_frames);
  BOOMPI_EXPECT(context, output.end_of_stream == input.end_of_stream);
  BOOMPI_EXPECT(context, output.HasValidLength());

  auto partial = MakeFrame(1U, 2U, 7777, 63U, true, 42U, 960U);
  partial.metadata.sequence = 8U;
  partial.metadata.monotonic_timestamp_us = 21000U;
  result = unity.Process(partial, &output);
  BOOMPI_EXPECT(context, result.produced());
  BOOMPI_EXPECT(context, output.valid_samples == 63U);
  BOOMPI_EXPECT(context, output.source_offset_sample_frames == 960U);
  BOOMPI_EXPECT(context, output.end_of_stream);
  BOOMPI_EXPECT(context, MetadataEqual(output.metadata, partial.metadata));
  BOOMPI_EXPECT(context, AllValidSamplesEqual(output, 7777));
  bool tail_is_zero = true;
  for (std::size_t sample = output.valid_samples;
       sample < output.samples.size(); ++sample) {
    tail_is_zero = tail_is_zero && output.samples[sample] == 0;
  }
  BOOMPI_EXPECT(context, tail_is_zero);
  BOOMPI_EXPECT(context, output.HasValidLength());
  FillOutputSentinel(&output, 12345);
  result = unity.Process(MakeFrame(1U, 2U), &output);
  BOOMPI_EXPECT(context, result.code == PlaybackGainLimiterCode::kNotArmed);
  BOOMPI_EXPECT(context, !unity.Arm(Generation(1U, 2U)).ok());
  BOOMPI_EXPECT(context, unity.Arm(Generation(2U, 3U)).ok());
  unity.Disarm();

  auto mute_config = UnityConfig();
  mute_config.volume_percent = 0U;
  mute_config.speaker_gain_percent = 400U;
  PlaybackGainLimiter mute;
  BOOMPI_EXPECT(
      context, PlaybackGainLimiter::Create(mute_config, &mute).ok());
  BOOMPI_EXPECT(context, mute.Arm(Generation(1U, 2U)).ok());
  input = MakeFrame(1U, 2U,
                    std::numeric_limits<std::int16_t>::max());
  result = mute.Process(input, &output);
  BOOMPI_EXPECT(context, result.produced());
  BOOMPI_EXPECT(context, result.over_full_scale_samples == 0U);
  BOOMPI_EXPECT(context, result.limited_samples == 0U);
  BOOMPI_EXPECT(context, result.safety_saturated_samples == 0U);
  BOOMPI_EXPECT(context, AllValidSamplesEqual(output, 0));

  PlaybackGainLimiter default_volume;
  BOOMPI_EXPECT(
      context,
      PlaybackGainLimiter::Create(PlaybackGainLimiterConfig{},
                                  &default_volume)
          .ok());
  BOOMPI_EXPECT(context,
                default_volume.Arm(Generation(1U, 2U)).ok());
  input = MakeFrame(1U, 2U, 10000);
  result = default_volume.Process(input, &output);
  BOOMPI_EXPECT(context, result.produced());
  BOOMPI_EXPECT(context, AllValidSamplesEqual(output, 6000));
}

void TestGainLimiterAndExtremes(boompi::test::TestContext& context) {
  auto config = UnityConfig();
  config.speaker_gain_percent = 200U;
  PlaybackGainLimiter gain;
  BOOMPI_EXPECT(context, PlaybackGainLimiter::Create(config, &gain).ok());
  BOOMPI_EXPECT(context, gain.Arm(Generation(1U, 2U)).ok());
  auto input = MakeFrame(1U, 2U, 10000);
  PlaybackPcmFrame48k output{};
  auto result = gain.Process(input, &output);
  BOOMPI_EXPECT(context, result.produced());
  BOOMPI_EXPECT(context, result.over_full_scale_samples == 0U);
  BOOMPI_EXPECT(context, result.limited_samples == 0U);
  BOOMPI_EXPECT(context, result.safety_saturated_samples == 0U);
  BOOMPI_EXPECT(context, AllValidSamplesEqual(output, 20000));

  config.speaker_gain_percent = 400U;
  PlaybackGainLimiter maximum_gain;
  BOOMPI_EXPECT(
      context,
      PlaybackGainLimiter::Create(config, &maximum_gain).ok());
  BOOMPI_EXPECT(context,
                maximum_gain.Arm(Generation(1U, 2U)).ok());
  input = MakeFrame(1U, 2U, 0, 4U, true);
  input.samples[0U] = std::numeric_limits<std::int16_t>::max();
  input.samples[1U] = std::numeric_limits<std::int16_t>::min();
  input.samples[2U] = 10000;
  input.samples[3U] = -10000;
  result = maximum_gain.Process(input, &output);
  BOOMPI_EXPECT(context, result.produced());
  BOOMPI_EXPECT(context, result.over_full_scale_samples == 4U);
  BOOMPI_EXPECT(context, result.limited_samples == 4U);
  BOOMPI_EXPECT(context, result.safety_saturated_samples == 0U);
  BOOMPI_EXPECT(
      context,
      output.samples[0U] == std::numeric_limits<std::int16_t>::max());
  BOOMPI_EXPECT(
      context,
      output.samples[1U] == std::numeric_limits<std::int16_t>::min());
  BOOMPI_EXPECT(
      context,
      output.samples[2U] == 10000);
  BOOMPI_EXPECT(
      context,
      output.samples[3U] == -10000);

  config = UnityConfig();
  config.limiter_ceiling_percent = 50U;
  PlaybackGainLimiter half_ceiling;
  BOOMPI_EXPECT(
      context,
      PlaybackGainLimiter::Create(config, &half_ceiling).ok());
  BOOMPI_EXPECT(context,
                half_ceiling.Arm(Generation(1U, 2U)).ok());
  input = MakeFrame(1U, 2U, 0, 2U, true);
  input.samples[0U] = std::numeric_limits<std::int16_t>::max();
  input.samples[1U] = std::numeric_limits<std::int16_t>::min();
  result = half_ceiling.Process(input, &output);
  BOOMPI_EXPECT(context, result.produced());
  BOOMPI_EXPECT(context, result.over_full_scale_samples == 0U);
  BOOMPI_EXPECT(context, result.limited_samples == 2U);
  BOOMPI_EXPECT(context, result.safety_saturated_samples == 0U);
  BOOMPI_EXPECT(context, output.samples[0U] == 16384);
  BOOMPI_EXPECT(context, output.samples[1U] == -16384);

  PlaybackGainLimiter ratio_limiter;
  BOOMPI_EXPECT(
      context,
      PlaybackGainLimiter::Create(config, &ratio_limiter).ok());
  BOOMPI_EXPECT(context,
                ratio_limiter.Arm(Generation(1U, 2U)).ok());
  input = MakeFrame(1U, 2U, 0, 2U, true);
  input.samples[0U] = 20000;
  input.samples[1U] = -10000;
  result = ratio_limiter.Process(input, &output);
  BOOMPI_EXPECT(context, result.produced());
  BOOMPI_EXPECT(context, result.over_full_scale_samples == 0U);
  BOOMPI_EXPECT(context, result.limited_samples == 2U);
  BOOMPI_EXPECT(context, result.safety_saturated_samples == 0U);
  BOOMPI_EXPECT(context, output.samples[0U] == 16384);
  BOOMPI_EXPECT(context, output.samples[1U] == -8192);
  BOOMPI_EXPECT(context,
                output.samples[0U] == -2 * output.samples[1U]);

  config = UnityConfig();
  PlaybackGainLimiter wide_limiter;
  BOOMPI_EXPECT(
      context,
      PlaybackGainLimiter::Create(config, &wide_limiter).ok());
  BOOMPI_EXPECT(context,
                wide_limiter.Arm(Generation(1U, 2U)).ok());
  input = MakeFrame(1U, 2U, 0, 4U, true);
  input.samples[0U] = 60000;
  input.samples[1U] = -40000;
  input.samples[2U] = 30000;
  input.samples[3U] = -20000;
  result = wide_limiter.Process(input, &output);
  BOOMPI_EXPECT(context, result.produced());
  BOOMPI_EXPECT(context, result.over_full_scale_samples == 2U);
  BOOMPI_EXPECT(context, result.limited_samples == 4U);
  BOOMPI_EXPECT(context, result.safety_saturated_samples == 0U);
  BOOMPI_EXPECT(context,
                output.samples[0U] ==
                    std::numeric_limits<std::int16_t>::max());
  const std::int32_t wide_ratio_error =
      2 * static_cast<std::int32_t>(output.samples[0U]) +
      3 * static_cast<std::int32_t>(output.samples[1U]);
  BOOMPI_EXPECT(context, wide_ratio_error >= -2 && wide_ratio_error <= 2);
  const std::int32_t quiet_ratio_error =
      2 * static_cast<std::int32_t>(output.samples[2U]) +
      3 * static_cast<std::int32_t>(output.samples[3U]);
  BOOMPI_EXPECT(context, quiet_ratio_error >= -2 && quiet_ratio_error <= 2);
}

void ExpectFramesEqual(boompi::test::TestContext& context,
                       const PlaybackPcmFrame48k& lhs,
                       const PlaybackPcmFrame48k& rhs) {
  BOOMPI_EXPECT(context, lhs.samples == rhs.samples);
  BOOMPI_EXPECT(context, lhs.valid_samples == rhs.valid_samples);
  BOOMPI_EXPECT(context, lhs.end_of_stream == rhs.end_of_stream);
  BOOMPI_EXPECT(context, MetadataEqual(lhs.metadata, rhs.metadata));
}

void TestDuckAndRecoveryRamp(boompi::test::TestContext& context) {
  const auto config = UnityConfig();
  PlaybackGainLimiter candidate;
  PlaybackGainLimiter control;
  BOOMPI_EXPECT(
      context,
      PlaybackGainLimiter::Create(config, &candidate).ok());
  BOOMPI_EXPECT(
      context, PlaybackGainLimiter::Create(config, &control).ok());
  BOOMPI_EXPECT(context, candidate.Arm(Generation(1U, 2U)).ok());
  BOOMPI_EXPECT(context, control.Arm(Generation(1U, 2U)).ok());
  BOOMPI_EXPECT(context, candidate.SetDucked(true).ok());
  BOOMPI_EXPECT(context, control.SetDucked(true).ok());

  std::array<PlaybackPcmFrame48k, 4U> duck_outputs{};
  std::array<PlaybackPcmFrame48k, 4U> control_outputs{};
  for (std::size_t frame_index = 0U;
       frame_index < duck_outputs.size(); ++frame_index) {
    auto input = MakeFrame(1U, 2U, 10000);
    input.metadata.sequence = static_cast<std::uint32_t>(frame_index);
    input.metadata.monotonic_timestamp_us =
        1000U + static_cast<std::uint64_t>(frame_index) * 20000U;
    const auto candidate_result =
        candidate.Process(input, &duck_outputs[frame_index]);
    const auto control_result =
        control.Process(input, &control_outputs[frame_index]);
    BOOMPI_EXPECT(context, candidate_result.produced());
    BOOMPI_EXPECT(context, control_result.produced());
    BOOMPI_EXPECT(context,
                  candidate_result.over_full_scale_samples == 0U);
    BOOMPI_EXPECT(context, candidate_result.limited_samples == 0U);
    BOOMPI_EXPECT(context,
                  candidate_result.safety_saturated_samples == 0U);
    ExpectFramesEqual(context, duck_outputs[frame_index],
                      control_outputs[frame_index]);
    if (frame_index == 0U) {
      BOOMPI_EXPECT(context, candidate.SetDucked(true).ok());
    }
  }

  std::int16_t previous = 10000;
  for (const auto& frame : duck_outputs) {
    for (const std::int16_t sample : frame.samples) {
      BOOMPI_EXPECT(context, sample <= previous);
      previous = sample;
    }
  }
  BOOMPI_EXPECT(context, duck_outputs[2U].samples.back() > 2500);
  BOOMPI_EXPECT(context, duck_outputs[3U].samples.back() == 2500);
  BOOMPI_EXPECT(context,
                duck_outputs[0U].samples.back() >=
                    duck_outputs[1U].samples.front());

  BOOMPI_EXPECT(context, candidate.SetDucked(false).ok());
  BOOMPI_EXPECT(context, control.SetDucked(false).ok());
  std::array<PlaybackPcmFrame48k, 4U> recovery_outputs{};
  for (std::size_t frame_index = 0U;
       frame_index < recovery_outputs.size(); ++frame_index) {
    auto input = MakeFrame(1U, 2U, 10000);
    input.metadata.sequence =
        static_cast<std::uint32_t>(frame_index + 4U);
    input.metadata.monotonic_timestamp_us =
        81000U + static_cast<std::uint64_t>(frame_index) * 20000U;
    PlaybackPcmFrame48k control_output{};
    BOOMPI_EXPECT(
        context,
        candidate.Process(input, &recovery_outputs[frame_index]).produced());
    BOOMPI_EXPECT(context,
                  control.Process(input, &control_output).produced());
    ExpectFramesEqual(context, recovery_outputs[frame_index], control_output);
    if (frame_index == 0U) {
      BOOMPI_EXPECT(context, candidate.SetDucked(false).ok());
    }
  }

  previous = 2500;
  for (const auto& frame : recovery_outputs) {
    for (const std::int16_t sample : frame.samples) {
      BOOMPI_EXPECT(context, sample >= previous);
      previous = sample;
    }
  }
  BOOMPI_EXPECT(context, recovery_outputs[2U].samples.back() < 10000);
  BOOMPI_EXPECT(context, recovery_outputs[3U].samples.back() == 10000);
}

void TestResetFaultsAndFailedCallsDoNotAdvance(
    boompi::test::TestContext& context) {
  const auto config = UnityConfig();
  PlaybackGainLimiter candidate;
  PlaybackGainLimiter control;
  BOOMPI_EXPECT(
      context,
      PlaybackGainLimiter::Create(config, &candidate).ok());
  BOOMPI_EXPECT(
      context, PlaybackGainLimiter::Create(config, &control).ok());
  BOOMPI_EXPECT(context, candidate.Arm(Generation(1U, 2U)).ok());
  BOOMPI_EXPECT(context, control.Arm(Generation(1U, 2U)).ok());
  BOOMPI_EXPECT(context, candidate.SetDucked(true).ok());
  BOOMPI_EXPECT(context, control.SetDucked(true).ok());

  auto input = MakeFrame(1U, 2U, 10000);
  auto result = candidate.Process(input, nullptr);
  BOOMPI_EXPECT(context,
                result.code == PlaybackGainLimiterCode::kInvalidOutput);
  BOOMPI_EXPECT(context, !result.produced());

  constexpr std::int16_t kSentinel = 23456;
  PlaybackPcmFrame48k failed_output{};
  FillOutputSentinel(&failed_output, kSentinel);
  auto stale = input;
  stale.metadata.epoch = 9U;
  result = candidate.Process(stale, &failed_output);
  BOOMPI_EXPECT(context,
                result.code == PlaybackGainLimiterCode::kEpochMismatch);
  ExpectInvalidHeaderPreservingPcm(context, failed_output, kSentinel);

  stale = input;
  stale.metadata.stream_id = 9U;
  FillOutputSentinel(&failed_output, kSentinel);
  result = candidate.Process(stale, &failed_output);
  BOOMPI_EXPECT(context,
                result.code == PlaybackGainLimiterCode::kStreamMismatch);
  ExpectInvalidHeaderPreservingPcm(context, failed_output, kSentinel);

  stale = input;
  stale.metadata.turn_id = 9U;
  FillOutputSentinel(&failed_output, kSentinel);
  result = candidate.Process(stale, &failed_output);
  BOOMPI_EXPECT(context,
                result.code == PlaybackGainLimiterCode::kTurnMismatch);
  ExpectInvalidHeaderPreservingPcm(context, failed_output, kSentinel);

  auto invalid = input;
  invalid.metadata.turn_id = 0U;
  FillOutputSentinel(&failed_output, kSentinel);
  result = candidate.Process(invalid, &failed_output);
  BOOMPI_EXPECT(context,
                result.code == PlaybackGainLimiterCode::kInvalidInput);
  ExpectInvalidHeaderPreservingPcm(context, failed_output, kSentinel);

  PlaybackPcmFrame48k candidate_output{};
  PlaybackPcmFrame48k control_output{};
  BOOMPI_EXPECT(context,
                candidate.Process(input, &candidate_output).produced());
  BOOMPI_EXPECT(context, control.Process(input, &control_output).produced());
  ExpectFramesEqual(context, candidate_output, control_output);

  BOOMPI_EXPECT(context, !candidate.Arm(Generation(2U, 2U)).ok());
  candidate.Disarm();
  BOOMPI_EXPECT(context, !candidate.Arm(Generation(1U, 2U)).ok());
  BOOMPI_EXPECT(context, candidate.Arm(Generation(2U, 2U)).ok());
  auto next_generation = MakeFrame(2U, 2U, 10000);
  BOOMPI_EXPECT(
      context,
      candidate.Process(next_generation, &candidate_output).produced());
  BOOMPI_EXPECT(context, AllValidSamplesEqual(candidate_output, 10000));

  auto discontinuity = next_generation;
  discontinuity.metadata.discontinuity = true;
  FillOutputSentinel(&failed_output, kSentinel);
  result = candidate.Process(discontinuity, &failed_output);
  BOOMPI_EXPECT(context,
                result.code == PlaybackGainLimiterCode::kDiscontinuity);
  BOOMPI_EXPECT(context, result.requires_new_generation());
  ExpectInvalidHeaderPreservingPcm(context, failed_output, kSentinel);
  FillOutputSentinel(&failed_output, kSentinel);
  result = candidate.Process(next_generation, &failed_output);
  BOOMPI_EXPECT(context, result.code == PlaybackGainLimiterCode::kFaulted);
  BOOMPI_EXPECT(context, result.requires_new_generation());
  ExpectInvalidHeaderPreservingPcm(context, failed_output, kSentinel);
  BOOMPI_EXPECT(context, !candidate.SetDucked(true).ok());

  candidate.Disarm();
  BOOMPI_EXPECT(context, candidate.Arm(Generation(3U, 3U)).ok());
  next_generation = MakeFrame(3U, 3U, 10000);
  result = candidate.Process(next_generation, &candidate_output);
  BOOMPI_EXPECT(context, result.produced());
  BOOMPI_EXPECT(context, AllValidSamplesEqual(candidate_output, 10000));

  candidate.Disarm();
  FillOutputSentinel(&failed_output, kSentinel);
  result = candidate.Process(next_generation, &failed_output);
  BOOMPI_EXPECT(context, result.code == PlaybackGainLimiterCode::kNotArmed);
  ExpectInvalidHeaderPreservingPcm(context, failed_output, kSentinel);
  BOOMPI_EXPECT(context, !candidate.Arm(Generation(3U, 3U)).ok());
  BOOMPI_EXPECT(context, candidate.Arm(Generation(4U, 3U)).ok());

}

void TestImmediateRamp(boompi::test::TestContext& context) {
  auto config = UnityConfig();
  config.duck_target_percent = 0U;
  config.duck_ramp_ms = 0U;
  config.recovery_ramp_ms = 0U;
  PlaybackGainLimiter limiter;
  BOOMPI_EXPECT(
      context, PlaybackGainLimiter::Create(config, &limiter).ok());
  BOOMPI_EXPECT(context, limiter.Arm(Generation(1U, 2U)).ok());
  BOOMPI_EXPECT(context, limiter.SetDucked(true).ok());
  auto input = MakeFrame(1U, 2U, 10000);
  PlaybackPcmFrame48k output{};
  BOOMPI_EXPECT(context, limiter.Process(input, &output).produced());
  BOOMPI_EXPECT(context, AllValidSamplesEqual(output, 0));
  BOOMPI_EXPECT(context, limiter.SetDucked(false).ok());
  BOOMPI_EXPECT(context, limiter.Process(input, &output).produced());
  BOOMPI_EXPECT(context, AllValidSamplesEqual(output, 10000));
}

void TestLimiterReleaseAcrossChunks(boompi::test::TestContext& context) {
  auto config = UnityConfig();
  config.limiter_ceiling_percent = 50U;
  config.limiter_release_ms = 80U;
  PlaybackGainLimiter limiter;
  BOOMPI_EXPECT(
      context, PlaybackGainLimiter::Create(config, &limiter).ok());
  BOOMPI_EXPECT(context, limiter.Arm(Generation(10U, 20U)).ok());

  auto input = MakeFrame(
      10U, 20U, std::numeric_limits<std::int16_t>::max());
  PlaybackPcmFrame48k output{};
  auto result = limiter.Process(input, &output);
  BOOMPI_EXPECT(context, result.produced());
  BOOMPI_EXPECT(context,
                result.limited_samples ==
                    ResampledPcmFrame48k::kFrameSamples);
  BOOMPI_EXPECT(context, AllValidSamplesEqual(output, 16384));

  std::uint32_t recovery_limited_samples = 0U;
  std::int16_t previous = 0;
  for (std::uint32_t frame_index = 0U; frame_index < 4U; ++frame_index) {
    input = MakeFrame(10U, 20U, 10000);
    input.metadata.sequence = frame_index + 1U;
    result = limiter.Process(input, &output);
    BOOMPI_EXPECT(context, result.produced());
    BOOMPI_EXPECT(context, result.safety_saturated_samples == 0U);
    recovery_limited_samples += result.limited_samples;
    for (std::size_t sample = 0U; sample < output.valid_samples; ++sample) {
      BOOMPI_EXPECT(context, output.samples[sample] >= previous);
      previous = output.samples[sample];
    }
  }
  BOOMPI_EXPECT(context, output.samples.back() == 10000);
  BOOMPI_EXPECT(context, recovery_limited_samples == 3839U);

  limiter.Disarm();
  BOOMPI_EXPECT(context, limiter.Arm(Generation(11U, 21U)).ok());
  input = MakeFrame(
      11U, 21U, std::numeric_limits<std::int16_t>::max());
  BOOMPI_EXPECT(context, limiter.Process(input, &output).produced());
  input = MakeFrame(11U, 21U, 10000);
  input.metadata.sequence = 1U;
  BOOMPI_EXPECT(context, limiter.Process(input, &output).produced());
  BOOMPI_EXPECT(context, output.samples.back() > 5000);
  input = MakeFrame(11U, 21U, 65536);
  input.metadata.sequence = 2U;
  result = limiter.Process(input, &output);
  BOOMPI_EXPECT(context, result.produced());
  BOOMPI_EXPECT(context,
                result.over_full_scale_samples ==
                    ResampledPcmFrame48k::kFrameSamples);
  BOOMPI_EXPECT(context,
                result.limited_samples ==
                    ResampledPcmFrame48k::kFrameSamples);
  BOOMPI_EXPECT(context, result.safety_saturated_samples == 0U);
  BOOMPI_EXPECT(context, AllValidSamplesEqual(output, 16384));

  limiter.Disarm();
  BOOMPI_EXPECT(context, limiter.Arm(Generation(12U, 22U)).ok());
  input = MakeFrame(12U, 22U,
                    std::numeric_limits<std::int16_t>::max(), 100U,
                    false);
  BOOMPI_EXPECT(context, limiter.Process(input, &output).produced());
  input = MakeFrame(12U, 22U, 10000, 63U, true, 42U, 100U);
  input.metadata.sequence = 1U;
  result = limiter.Process(input, &output);
  BOOMPI_EXPECT(context, result.produced());
  BOOMPI_EXPECT(context, output.source_offset_sample_frames == 100U);
  BOOMPI_EXPECT(context, output.end_of_stream);
  BOOMPI_EXPECT(context, output.samples.front() < 6000);
  const std::int16_t drain_last =
      output.samples[output.valid_samples - 1U];
  BOOMPI_EXPECT(context, drain_last < 6000);
  BOOMPI_EXPECT(context, drain_last >= output.samples.front());

  config.limiter_release_ms = 0U;
  PlaybackGainLimiter immediate;
  BOOMPI_EXPECT(
      context, PlaybackGainLimiter::Create(config, &immediate).ok());
  BOOMPI_EXPECT(context, immediate.Arm(Generation(13U, 23U)).ok());
  input = MakeFrame(
      13U, 23U, std::numeric_limits<std::int16_t>::max());
  BOOMPI_EXPECT(context, immediate.Process(input, &output).produced());
  input = MakeFrame(13U, 23U, 10000);
  input.metadata.sequence = 1U;
  result = immediate.Process(input, &output);
  BOOMPI_EXPECT(context, result.produced());
  BOOMPI_EXPECT(context, result.limited_samples == 0U);
  BOOMPI_EXPECT(context, AllValidSamplesEqual(output, 10000));
}

void TestDeterministicWideBoundarySweep(
    boompi::test::TestContext& context) {
  constexpr std::array<std::uint8_t, 4U> kCeilings{{1U, 50U, 95U, 100U}};
  constexpr std::array<std::uint16_t, 3U> kSpeakerGains{{0U, 100U, 400U}};
  std::uint32_t case_index = 0U;
  for (const std::uint8_t ceiling_percent : kCeilings) {
    for (const std::uint16_t speaker_gain_percent : kSpeakerGains) {
      auto config = UnityConfig();
      config.speaker_gain_percent = speaker_gain_percent;
      config.limiter_ceiling_percent = ceiling_percent;
      config.duck_ramp_ms = 0U;
      PlaybackGainLimiter limiter;
      BOOMPI_EXPECT(
          context, PlaybackGainLimiter::Create(config, &limiter).ok());
      const auto generation =
          Generation(100U + case_index, 200U + case_index);
      BOOMPI_EXPECT(context, limiter.Arm(generation).ok());
      const bool exercise_duck =
          ceiling_percent == 95U && speaker_gain_percent == 400U;
      if (exercise_duck) {
        BOOMPI_EXPECT(context, limiter.SetDucked(true).ok());
      }

      constexpr std::uint16_t kValidSamples = 257U;
      auto input = MakeFrame(generation.epoch, generation.stream_id, 0,
                             kValidSamples, true, generation.turn_id);
      std::uint32_t prng_state = UINT32_C(0x8f31a5c7) + case_index;
      for (std::size_t sample = 0U; sample < kValidSamples; ++sample) {
        prng_state = prng_state * UINT32_C(1664525) +
                     UINT32_C(1013904223);
        const std::int64_t centered =
            static_cast<std::int64_t>(prng_state) - INT64_C(2147483648);
        input.samples[sample] = static_cast<std::int32_t>(centered);
      }
      input.samples[0U] = std::numeric_limits<std::int32_t>::min();
      input.samples[1U] = std::numeric_limits<std::int32_t>::max();

      PlaybackPcmFrame48k output{};
      const auto result = limiter.Process(input, &output);
      BOOMPI_EXPECT(context, result.produced());
      BOOMPI_EXPECT(context, result.safety_saturated_samples == 0U);
      BOOMPI_EXPECT(context, output.HasValidLength());
      const std::int32_t positive_ceiling =
          (32767 * static_cast<std::int32_t>(ceiling_percent) + 50) / 100;
      const std::int32_t negative_ceiling_magnitude =
          (32768 * static_cast<std::int32_t>(ceiling_percent) + 50) / 100;
      bool samples_are_bounded = true;
      for (std::size_t sample = 0U; sample < output.valid_samples; ++sample) {
        const std::int32_t value = output.samples[sample];
        samples_are_bounded = samples_are_bounded &&
                              value <= positive_ceiling &&
                              value >= -negative_ceiling_magnitude;
      }
      bool tail_is_zero = true;
      for (std::size_t sample = output.valid_samples;
           sample < output.samples.size(); ++sample) {
        tail_is_zero = tail_is_zero && output.samples[sample] == 0;
      }
      BOOMPI_EXPECT(context, samples_are_bounded);
      BOOMPI_EXPECT(context, tail_is_zero);
      ++case_index;
    }
  }
}

}  // namespace

int main() {
  boompi::test::TestContext context;
  TestConfigurationValidation(context);
  TestUnityMuteAndMetadata(context);
  TestGainLimiterAndExtremes(context);
  TestDuckAndRecoveryRamp(context);
  TestResetFaultsAndFailedCallsDoNotAdvance(context);
  TestImmediateRamp(context);
  TestLimiterReleaseAcrossChunks(context);
  TestDeterministicWideBoundarySweep(context);

  if (context.failures() != 0U) {
    std::cerr << context.failures()
              << " playback gain/limiter expectation(s) failed\n";
    return 1;
  }
  std::cout << "boomPI playback gain/limiter tests passed\n";
  return 0;
}
