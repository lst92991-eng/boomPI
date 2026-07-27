#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <type_traits>
#include <utility>

#include "boompi/audio/playback_renderer_24_to_48.h"
#include "boompi/test/test_context.h"

#define BOOMPI_EXPECT(context, expression) \
  (context).Expect((expression), #expression, __FILE__, __LINE__)

namespace {

using boompi::StatusCode;
using boompi::audio::PlaybackGainLimiterConfig;
using boompi::audio::PlaybackGeneration;
using boompi::audio::PlaybackPcmFrame48k;
using boompi::audio::PlaybackRenderCode;
using boompi::audio::PlaybackRenderResult;
using boompi::audio::PlaybackRenderer24To48;
using boompi::audio::SampleFormat;
using boompi::audio::TtsPcmFrame24k;

static_assert(std::is_trivial<PlaybackRenderResult>::value,
              "renderer result must remain a POD value");
static_assert(std::is_standard_layout<PlaybackRenderResult>::value,
              "renderer result must remain standard-layout");
static_assert(
    noexcept(std::declval<PlaybackRenderer24To48&>().Process(
        std::declval<const TtsPcmFrame24k&>(),
        std::declval<PlaybackPcmFrame48k*>())),
    "renderer hot path must remain noexcept");
static_assert(
    noexcept(std::declval<PlaybackRenderer24To48&>().Drain(
        std::declval<PlaybackPcmFrame48k*>())),
    "renderer drain path must remain noexcept");

PlaybackGeneration Generation(const std::uint32_t epoch,
                              const std::uint32_t turn_id,
                              const std::uint32_t stream_id) {
  return {epoch, turn_id, stream_id};
}

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

TtsPcmFrame24k MakeFrame(
    const PlaybackGeneration& generation, const std::uint32_t sequence,
    const std::uint64_t timestamp_us, const std::int16_t sample_value = 10000,
    const std::uint16_t valid_source_samples =
        TtsPcmFrame24k::kFrameSamples,
    const bool end_of_stream = false) {
  TtsPcmFrame24k frame{};
  frame.format = {TtsPcmFrame24k::kSampleRateHz,
                  TtsPcmFrame24k::kFrameDurationMs,
                  TtsPcmFrame24k::kChannelCount,
                  TtsPcmFrame24k::kSampleFormat};
  frame.metadata.monotonic_timestamp_us = timestamp_us;
  frame.metadata.sequence = sequence;
  frame.metadata.epoch = generation.epoch;
  frame.metadata.turn_id = generation.turn_id;
  frame.metadata.stream_id = generation.stream_id;
  frame.valid_source_samples = valid_source_samples;
  frame.end_of_stream = end_of_stream;
  for (std::size_t sample = 0U; sample < frame.samples.size(); ++sample) {
    frame.samples[sample] =
        sample < valid_source_samples ? sample_value : 0;
  }
  return frame;
}

bool MetadataEqual(const boompi::audio::AudioFrameMetadata& lhs,
                   const boompi::audio::AudioFrameMetadata& rhs) {
  return lhs.monotonic_timestamp_us == rhs.monotonic_timestamp_us &&
         lhs.sequence == rhs.sequence && lhs.stream_id == rhs.stream_id &&
         lhs.turn_id == rhs.turn_id && lhs.epoch == rhs.epoch &&
         lhs.discontinuity == rhs.discontinuity;
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

bool TailIsZero(const PlaybackPcmFrame48k& frame) {
  for (std::size_t sample = frame.valid_samples;
       sample < frame.samples.size(); ++sample) {
    if (frame.samples[sample] != 0) {
      return false;
    }
  }
  return true;
}

void FillOutputSentinel(PlaybackPcmFrame48k* const output,
                        const std::int16_t sentinel) {
  *output = PlaybackPcmFrame48k{};
  output->format = {PlaybackPcmFrame48k::kSampleRateHz,
                    PlaybackPcmFrame48k::kFrameDurationMs,
                    PlaybackPcmFrame48k::kChannelCount,
                    SampleFormat::kPcmS16Le};
  output->metadata.monotonic_timestamp_us = 1U;
  output->metadata.sequence = 1U;
  output->metadata.epoch = 91U;
  output->metadata.turn_id = 92U;
  output->metadata.stream_id = 93U;
  output->valid_samples = PlaybackPcmFrame48k::kFrameSamples;
  output->samples.fill(sentinel);
}

void ExpectInvalidHeaderPreservingPcm(
    boompi::test::TestContext& context,
    const PlaybackPcmFrame48k& output, const std::int16_t sentinel) {
  BOOMPI_EXPECT(context, !output.HasValidLength());
  BOOMPI_EXPECT(context, output.format.sample_rate_hz == 0U);
  BOOMPI_EXPECT(context, output.format.frame_duration_ms == 0U);
  BOOMPI_EXPECT(context, output.format.channels == 0U);
  BOOMPI_EXPECT(context, output.metadata.epoch == 0U);
  BOOMPI_EXPECT(context, output.metadata.turn_id == 0U);
  BOOMPI_EXPECT(context, output.metadata.stream_id == 0U);
  BOOMPI_EXPECT(context, output.source_offset_sample_frames == 0U);
  BOOMPI_EXPECT(context, output.valid_samples == 0U);
  BOOMPI_EXPECT(context, !output.end_of_stream);
  BOOMPI_EXPECT(context, output.samples.front() == sentinel);
  BOOMPI_EXPECT(context, output.samples.back() == sentinel);
}

void TestConfigurationAndTransactionalArm(
    boompi::test::TestContext& context) {
  const auto generation = Generation(1U, 2U, 3U);
  PlaybackRenderer24To48 renderer;
  BOOMPI_EXPECT(context, !renderer.Arm(generation).ok());

  constexpr std::int16_t kSentinel = 23456;
  PlaybackPcmFrame48k output{};
  FillOutputSentinel(&output, kSentinel);
  auto result =
      renderer.Process(MakeFrame(generation, 0U, 1000000U), &output);
  BOOMPI_EXPECT(context, result.code == PlaybackRenderCode::kNotConfigured);
  ExpectInvalidHeaderPreservingPcm(context, output, kSentinel);

  auto invalid_config = UnityConfig();
  invalid_config.speaker_gain_percent = 401U;
  auto status = PlaybackRenderer24To48::Create(invalid_config, &renderer);
  BOOMPI_EXPECT(context, !status.ok());
  BOOMPI_EXPECT(context, status.code() == StatusCode::kInvalidArgument);
  status = PlaybackRenderer24To48::Create(UnityConfig(), &renderer);
  BOOMPI_EXPECT(context, status.ok());
  BOOMPI_EXPECT(context,
                !PlaybackRenderer24To48::Create(UnityConfig(), &renderer)
                     .ok());
  BOOMPI_EXPECT(context, !renderer.SetDucked(true).ok());
  BOOMPI_EXPECT(context, !renderer.Arm({0U, 2U, 3U}).ok());
  BOOMPI_EXPECT(context, renderer.Arm(generation).ok());

  const auto next_generation = Generation(4U, 5U, 6U);
  BOOMPI_EXPECT(context, !renderer.Arm(next_generation).ok());
  FillOutputSentinel(&output, kSentinel);
  result = renderer.Drain(&output);
  BOOMPI_EXPECT(context,
                result.code == PlaybackRenderCode::kNoDrainPending);
  ExpectInvalidHeaderPreservingPcm(context, output, kSentinel);
  result = renderer.Process(MakeFrame(generation, 0U, 1000000U), &output);
  BOOMPI_EXPECT(context, result.produced());

  renderer.Disarm();
  BOOMPI_EXPECT(context,
                renderer.Process(MakeFrame(generation, 1U, 1020000U),
                                 &output)
                        .code == PlaybackRenderCode::kNotArmed);
  BOOMPI_EXPECT(context, !renderer.Arm(generation).ok());
  BOOMPI_EXPECT(context, renderer.Arm(next_generation).ok());
  renderer.Disarm();
}

void TestUnityGainLimiterAndMetadata(boompi::test::TestContext& context) {
  const auto generation = Generation(10U, 11U, 12U);
  PlaybackRenderer24To48 unity;
  BOOMPI_EXPECT(
      context, PlaybackRenderer24To48::Create(UnityConfig(), &unity).ok());
  BOOMPI_EXPECT(context, unity.Arm(generation).ok());
  PlaybackPcmFrame48k output{};
  auto first = MakeFrame(generation, 0U, 2000000U, 10000);
  auto result = unity.Process(first, &output);
  BOOMPI_EXPECT(context, result.produced());
  BOOMPI_EXPECT(context, !result.drain_required);
  auto second = MakeFrame(generation, 1U, 2020000U, 10000);
  result = unity.Process(second, &output);
  BOOMPI_EXPECT(context, result.produced());
  BOOMPI_EXPECT(context, result.over_full_scale_samples == 0U);
  BOOMPI_EXPECT(context, result.limited_samples == 0U);
  BOOMPI_EXPECT(context, result.safety_saturated_samples == 0U);
  BOOMPI_EXPECT(context, output.HasValidLength());
  BOOMPI_EXPECT(context, AllValidSamplesEqual(output, 10000));
  BOOMPI_EXPECT(context, MetadataEqual(output.metadata, second.metadata));
  BOOMPI_EXPECT(context, output.source_offset_sample_frames == 0U);
  BOOMPI_EXPECT(context, !output.end_of_stream);
  BOOMPI_EXPECT(context,
                output.format.sample_rate_hz ==
                    PlaybackPcmFrame48k::kSampleRateHz);

  auto gain_config = UnityConfig();
  gain_config.speaker_gain_percent = 200U;
  const auto gain_generation = Generation(13U, 14U, 15U);
  PlaybackRenderer24To48 gain;
  BOOMPI_EXPECT(
      context, PlaybackRenderer24To48::Create(gain_config, &gain).ok());
  BOOMPI_EXPECT(context, gain.Arm(gain_generation).ok());
  BOOMPI_EXPECT(
      context,
      gain.Process(MakeFrame(gain_generation, 0U, 3000000U), &output)
          .produced());
  result = gain.Process(
      MakeFrame(gain_generation, 1U, 3020000U), &output);
  BOOMPI_EXPECT(context, result.produced());
  BOOMPI_EXPECT(context, AllValidSamplesEqual(output, 20000));
  BOOMPI_EXPECT(context, result.limited_samples == 0U);

  gain_config.speaker_gain_percent = 400U;
  const auto limiter_generation = Generation(16U, 17U, 18U);
  PlaybackRenderer24To48 limiter;
  BOOMPI_EXPECT(
      context,
      PlaybackRenderer24To48::Create(gain_config, &limiter).ok());
  BOOMPI_EXPECT(context, limiter.Arm(limiter_generation).ok());
  BOOMPI_EXPECT(
      context,
      limiter
          .Process(MakeFrame(limiter_generation, 0U, 4000000U), &output)
          .produced());
  result = limiter.Process(
      MakeFrame(limiter_generation, 1U, 4020000U), &output);
  BOOMPI_EXPECT(context, result.produced());
  BOOMPI_EXPECT(context,
                result.over_full_scale_samples ==
                    PlaybackPcmFrame48k::kFrameSamples);
  BOOMPI_EXPECT(context, result.limited_samples > 0U);
  BOOMPI_EXPECT(context, result.safety_saturated_samples == 0U);
  bool all_bounded = true;
  for (std::size_t sample = 0U; sample < output.valid_samples; ++sample) {
    all_bounded = all_bounded &&
                  output.samples[sample] <=
                      std::numeric_limits<std::int16_t>::max() &&
                  output.samples[sample] >=
                      std::numeric_limits<std::int16_t>::min();
  }
  BOOMPI_EXPECT(context, all_bounded);
}

void TestDuckAcrossFrames(boompi::test::TestContext& context) {
  const auto generation = Generation(20U, 21U, 22U);
  PlaybackRenderer24To48 renderer;
  BOOMPI_EXPECT(
      context,
      PlaybackRenderer24To48::Create(UnityConfig(), &renderer).ok());
  BOOMPI_EXPECT(context, renderer.Arm(generation).ok());
  PlaybackPcmFrame48k output{};
  BOOMPI_EXPECT(
      context,
      renderer.Process(MakeFrame(generation, 0U, 5000000U), &output)
          .produced());
  BOOMPI_EXPECT(context, renderer.SetDucked(true).ok());

  std::array<PlaybackPcmFrame48k, 4U> ducked{};
  for (std::size_t frame = 0U; frame < ducked.size(); ++frame) {
    const auto input = MakeFrame(
        generation, static_cast<std::uint32_t>(frame + 1U),
        5020000U + static_cast<std::uint64_t>(frame) * 20000U);
    BOOMPI_EXPECT(context, renderer.Process(input, &ducked[frame]).produced());
    if (frame == 0U) {
      BOOMPI_EXPECT(context, renderer.SetDucked(true).ok());
    }
  }
  std::int16_t previous = 10000;
  for (const auto& frame : ducked) {
    for (const std::int16_t sample : frame.samples) {
      BOOMPI_EXPECT(context, sample <= previous);
      previous = sample;
    }
  }
  BOOMPI_EXPECT(context, ducked.back().samples.back() == 2500);

  BOOMPI_EXPECT(context, renderer.SetDucked(false).ok());
  std::array<PlaybackPcmFrame48k, 4U> recovered{};
  for (std::size_t frame = 0U; frame < recovered.size(); ++frame) {
    const auto input = MakeFrame(
        generation, static_cast<std::uint32_t>(frame + 5U),
        5100000U + static_cast<std::uint64_t>(frame) * 20000U);
    BOOMPI_EXPECT(
        context, renderer.Process(input, &recovered[frame]).produced());
  }
  previous = 2500;
  for (const auto& frame : recovered) {
    for (const std::int16_t sample : frame.samples) {
      BOOMPI_EXPECT(context, sample >= previous);
      previous = sample;
    }
  }
  BOOMPI_EXPECT(context, recovered.back().samples.back() == 10000);
}

void TestNullAndStaleDoNotAdvance(boompi::test::TestContext& context) {
  const auto generation = Generation(30U, 31U, 32U);
  PlaybackRenderer24To48 candidate;
  PlaybackRenderer24To48 control;
  BOOMPI_EXPECT(
      context,
      PlaybackRenderer24To48::Create(UnityConfig(), &candidate).ok());
  BOOMPI_EXPECT(
      context,
      PlaybackRenderer24To48::Create(UnityConfig(), &control).ok());
  BOOMPI_EXPECT(context, candidate.Arm(generation).ok());
  BOOMPI_EXPECT(context, control.Arm(generation).ok());
  PlaybackPcmFrame48k candidate_output{};
  PlaybackPcmFrame48k control_output{};
  const auto warmup = MakeFrame(generation, 0U, 6000000U);
  BOOMPI_EXPECT(context, candidate.Process(warmup, &candidate_output).produced());
  BOOMPI_EXPECT(context, control.Process(warmup, &control_output).produced());
  BOOMPI_EXPECT(context, candidate.SetDucked(true).ok());
  BOOMPI_EXPECT(context, control.SetDucked(true).ok());

  const auto valid = MakeFrame(generation, 1U, 6020000U);
  auto result = candidate.Process(valid, nullptr);
  BOOMPI_EXPECT(context, result.code == PlaybackRenderCode::kInvalidOutput);

  constexpr std::int16_t kSentinel = 12345;
  auto stale = valid;
  stale.metadata.epoch += 1U;
  FillOutputSentinel(&candidate_output, kSentinel);
  result = candidate.Process(stale, &candidate_output);
  BOOMPI_EXPECT(context, result.code == PlaybackRenderCode::kEpochMismatch);
  ExpectInvalidHeaderPreservingPcm(context, candidate_output, kSentinel);
  stale = valid;
  stale.metadata.stream_id += 1U;
  FillOutputSentinel(&candidate_output, kSentinel);
  result = candidate.Process(stale, &candidate_output);
  BOOMPI_EXPECT(context, result.code == PlaybackRenderCode::kStreamMismatch);
  ExpectInvalidHeaderPreservingPcm(context, candidate_output, kSentinel);
  stale = valid;
  stale.metadata.turn_id += 1U;
  FillOutputSentinel(&candidate_output, kSentinel);
  result = candidate.Process(stale, &candidate_output);
  BOOMPI_EXPECT(context, result.code == PlaybackRenderCode::kTurnMismatch);
  ExpectInvalidHeaderPreservingPcm(context, candidate_output, kSentinel);
  stale = valid;
  stale.metadata.turn_id = 0U;
  FillOutputSentinel(&candidate_output, kSentinel);
  result = candidate.Process(stale, &candidate_output);
  BOOMPI_EXPECT(context,
                result.code == PlaybackRenderCode::kInvalidGeneration);
  ExpectInvalidHeaderPreservingPcm(context, candidate_output, kSentinel);

  FillOutputSentinel(&candidate_output, kSentinel);
  result = candidate.Drain(&candidate_output);
  BOOMPI_EXPECT(context,
                result.code == PlaybackRenderCode::kNoDrainPending);
  ExpectInvalidHeaderPreservingPcm(context, candidate_output, kSentinel);
  const auto candidate_result = candidate.Process(valid, &candidate_output);
  const auto control_result = control.Process(valid, &control_output);
  BOOMPI_EXPECT(context, candidate_result.produced());
  BOOMPI_EXPECT(context, control_result.produced());
  BOOMPI_EXPECT(context, candidate_output.samples == control_output.samples);
  BOOMPI_EXPECT(context,
                candidate_result.limited_samples ==
                    control_result.limited_samples);
}

void TestFaultsRequireNewGeneration(boompi::test::TestContext& context) {
  PlaybackRenderer24To48 renderer;
  BOOMPI_EXPECT(
      context,
      PlaybackRenderer24To48::Create(UnityConfig(), &renderer).ok());
  PlaybackPcmFrame48k output{};

  auto generation = Generation(40U, 41U, 42U);
  BOOMPI_EXPECT(context, renderer.Arm(generation).ok());
  BOOMPI_EXPECT(
      context,
      renderer.Process(MakeFrame(generation, 0U, 7000000U), &output)
          .produced());
  auto malformed = MakeFrame(generation, 1U, 7020000U, 10000, 10U, false);
  constexpr std::int16_t kFaultSentinel = 19876;
  FillOutputSentinel(&output, kFaultSentinel);
  auto result = renderer.Process(malformed, &output);
  BOOMPI_EXPECT(context,
                result.code == PlaybackRenderCode::kInvalidInputFrame);
  BOOMPI_EXPECT(context, result.requires_new_generation());
  BOOMPI_EXPECT(context, !result.drain_required);
  ExpectInvalidHeaderPreservingPcm(context, output, kFaultSentinel);
  BOOMPI_EXPECT(
      context,
      renderer.Process(MakeFrame(generation, 1U, 7020000U), &output).code ==
          PlaybackRenderCode::kFaulted);
  BOOMPI_EXPECT(context, !renderer.SetDucked(true).ok());

  generation = Generation(43U, 44U, 45U);
  BOOMPI_EXPECT(context, renderer.Arm(generation).ok());
  auto discontinuity = MakeFrame(generation, 0U, 8000000U);
  discontinuity.metadata.discontinuity = true;
  result = renderer.Process(discontinuity, &output);
  BOOMPI_EXPECT(context, result.code == PlaybackRenderCode::kDiscontinuity);
  BOOMPI_EXPECT(context, result.requires_new_generation());

  generation = Generation(46U, 47U, 48U);
  BOOMPI_EXPECT(context, renderer.Arm(generation).ok());
  BOOMPI_EXPECT(
      context,
      renderer.Process(MakeFrame(generation, 0U, 9000000U), &output)
          .produced());
  result = renderer.Process(MakeFrame(generation, 2U, 9020000U), &output);
  BOOMPI_EXPECT(context, result.code == PlaybackRenderCode::kSequenceBreak);

  generation = Generation(49U, 50U, 51U);
  BOOMPI_EXPECT(context, renderer.Arm(generation).ok());
  BOOMPI_EXPECT(
      context,
      renderer.Process(MakeFrame(generation, 0U, 10000000U), &output)
          .produced());
  result = renderer.Process(
      MakeFrame(generation, 1U, 10000000U), &output);
  BOOMPI_EXPECT(
      context, result.code == PlaybackRenderCode::kTimestampNotIncreasing);

  generation = Generation(52U, 53U, 54U);
  BOOMPI_EXPECT(context, renderer.Arm(generation).ok());
  BOOMPI_EXPECT(
      context,
      renderer.Process(MakeFrame(generation, 0U, 11000000U), &output)
          .produced());
}

void TestExplicitEosDrain(boompi::test::TestContext& context) {
  const auto generation = Generation(60U, 61U, 62U);
  PlaybackRenderer24To48 renderer;
  BOOMPI_EXPECT(
      context,
      PlaybackRenderer24To48::Create(UnityConfig(), &renderer).ok());
  BOOMPI_EXPECT(context, renderer.Arm(generation).ok());
  constexpr std::uint16_t kValidSourceSamples = 73U;
  auto eos = MakeFrame(generation, 0U, 12000000U, 10000,
                       kValidSourceSamples, true);
  eos.samples[kValidSourceSamples - 1U] = 20000;
  PlaybackPcmFrame48k output{};
  auto result = renderer.Process(eos, &output);
  BOOMPI_EXPECT(context, result.produced());
  BOOMPI_EXPECT(context, result.drain_required);
  BOOMPI_EXPECT(context, output.HasValidLength());
  BOOMPI_EXPECT(context, !output.end_of_stream);
  BOOMPI_EXPECT(context,
                output.valid_samples == kValidSourceSamples * 2U);
  BOOMPI_EXPECT(context, output.source_offset_sample_frames == 0U);
  BOOMPI_EXPECT(context, MetadataEqual(output.metadata, eos.metadata));
  BOOMPI_EXPECT(context, TailIsZero(output));
  BOOMPI_EXPECT(context, !renderer.SetDucked(true).ok());

  constexpr std::int16_t kSentinel = 22222;
  auto stale = eos;
  stale.metadata.epoch += 1U;
  FillOutputSentinel(&output, kSentinel);
  result = renderer.Process(stale, &output);
  BOOMPI_EXPECT(context, result.code == PlaybackRenderCode::kEpochMismatch);
  BOOMPI_EXPECT(context, result.drain_required);
  ExpectInvalidHeaderPreservingPcm(context, output, kSentinel);

  stale = eos;
  stale.metadata.stream_id += 1U;
  FillOutputSentinel(&output, kSentinel);
  result = renderer.Process(stale, &output);
  BOOMPI_EXPECT(context, result.code == PlaybackRenderCode::kStreamMismatch);
  BOOMPI_EXPECT(context, result.drain_required);
  ExpectInvalidHeaderPreservingPcm(context, output, kSentinel);

  stale = eos;
  stale.metadata.turn_id = 0U;
  FillOutputSentinel(&output, kSentinel);
  result = renderer.Process(stale, &output);
  BOOMPI_EXPECT(context,
                result.code == PlaybackRenderCode::kInvalidGeneration);
  BOOMPI_EXPECT(context, result.drain_required);
  ExpectInvalidHeaderPreservingPcm(context, output, kSentinel);

  FillOutputSentinel(&output, kSentinel);
  result = renderer.Process(eos, &output);
  BOOMPI_EXPECT(context, result.code == PlaybackRenderCode::kDrainPending);
  BOOMPI_EXPECT(context, result.drain_required);
  ExpectInvalidHeaderPreservingPcm(context, output, kSentinel);
  BOOMPI_EXPECT(context,
                !renderer.Arm(Generation(63U, 64U, 65U)).ok());
  result = renderer.Drain(nullptr);
  BOOMPI_EXPECT(context, result.code == PlaybackRenderCode::kInvalidOutput);
  BOOMPI_EXPECT(context, result.drain_required);

  result = renderer.Drain(&output);
  BOOMPI_EXPECT(context, result.produced());
  BOOMPI_EXPECT(context, !result.drain_required);
  BOOMPI_EXPECT(context, output.HasValidLength());
  BOOMPI_EXPECT(context, output.end_of_stream);
  BOOMPI_EXPECT(context,
                output.valid_samples ==
                    boompi::audio::PlaybackResampler24To48::kDrainSamples);
  BOOMPI_EXPECT(
      context,
      output.source_offset_sample_frames == kValidSourceSamples * 2U);
  BOOMPI_EXPECT(context, MetadataEqual(output.metadata, eos.metadata));
  BOOMPI_EXPECT(context, TailIsZero(output));
  BOOMPI_EXPECT(context,
                renderer.Drain(&output).code ==
                    PlaybackRenderCode::kNotArmed);
  BOOMPI_EXPECT(context,
                renderer.Process(eos, &output).code ==
                    PlaybackRenderCode::kNotArmed);
  BOOMPI_EXPECT(context, !renderer.Arm(generation).ok());

  const auto next_generation = Generation(63U, 64U, 65U);
  BOOMPI_EXPECT(context, renderer.Arm(next_generation).ok());
  BOOMPI_EXPECT(
      context,
      renderer.Process(MakeFrame(next_generation, 0U, 13000000U), &output)
          .produced());
}

void TestFullFrameEosAndPendingDisarm(
    boompi::test::TestContext& context) {
  PlaybackRenderer24To48 renderer;
  BOOMPI_EXPECT(
      context,
      PlaybackRenderer24To48::Create(UnityConfig(), &renderer).ok());

  const auto full_generation = Generation(70U, 71U, 72U);
  BOOMPI_EXPECT(context, renderer.Arm(full_generation).ok());
  const auto full_eos = MakeFrame(
      full_generation, 0U, 14000000U, 7000,
      TtsPcmFrame24k::kFrameSamples, true);
  PlaybackPcmFrame48k output{};
  auto result = renderer.Process(full_eos, &output);
  BOOMPI_EXPECT(context, result.produced());
  BOOMPI_EXPECT(context, result.drain_required);
  BOOMPI_EXPECT(context,
                output.valid_samples == PlaybackPcmFrame48k::kFrameSamples);
  BOOMPI_EXPECT(context, output.source_offset_sample_frames == 0U);
  BOOMPI_EXPECT(context, !output.end_of_stream);

  result = renderer.Drain(&output);
  BOOMPI_EXPECT(context, result.produced());
  BOOMPI_EXPECT(context, !result.drain_required);
  BOOMPI_EXPECT(context, output.end_of_stream);
  BOOMPI_EXPECT(
      context,
      output.source_offset_sample_frames == PlaybackPcmFrame48k::kFrameSamples);
  BOOMPI_EXPECT(context,
                output.valid_samples ==
                    boompi::audio::PlaybackResampler24To48::kDrainSamples);
  BOOMPI_EXPECT(
      context,
      static_cast<std::uint32_t>(output.source_offset_sample_frames) +
              output.valid_samples ==
          PlaybackPcmFrame48k::kMaximumSourceSpanSampleFrames);

  const auto cancelled_generation = Generation(73U, 74U, 75U);
  BOOMPI_EXPECT(context, renderer.Arm(cancelled_generation).ok());
  const auto short_eos =
      MakeFrame(cancelled_generation, 0U, 15000000U, 9000, 11U, true);
  BOOMPI_EXPECT(context, renderer.Process(short_eos, &output).drain_required);
  renderer.Disarm();

  constexpr std::int16_t kSentinel = 17654;
  FillOutputSentinel(&output, kSentinel);
  result = renderer.Drain(&output);
  BOOMPI_EXPECT(context, result.code == PlaybackRenderCode::kNotArmed);
  BOOMPI_EXPECT(context, !result.drain_required);
  ExpectInvalidHeaderPreservingPcm(context, output, kSentinel);

  const auto recovery_generation = Generation(76U, 77U, 78U);
  BOOMPI_EXPECT(context, renderer.Arm(recovery_generation).ok());
  BOOMPI_EXPECT(
      context,
      renderer.Process(MakeFrame(recovery_generation, 0U, 16000000U),
                       &output)
          .produced());
}

void TestWideFirOvershootReachesLimiter(
    boompi::test::TestContext& context) {
  const auto generation = Generation(80U, 81U, 82U);
  PlaybackRenderer24To48 renderer;
  auto config = UnityConfig();
  config.limiter_ceiling_percent = 95U;
  BOOMPI_EXPECT(
      context,
      PlaybackRenderer24To48::Create(config, &renderer).ok());
  BOOMPI_EXPECT(context, renderer.Arm(generation).ok());

  auto input = MakeFrame(generation, 0U, 17000000U, 0);
  constexpr std::size_t kTargetSourceSample = 100U;
  constexpr std::size_t kOddPhaseTapCount =
      boompi::audio::PlaybackResampler24To48::kOddPhaseTapCount;
  for (std::size_t phase_tap = 0U; phase_tap < kOddPhaseTapCount;
       ++phase_tap) {
    const bool coefficient_is_positive =
        phase_tap < (kOddPhaseTapCount / 2U)
            ? (phase_tap % 2U) == 1U
            : (phase_tap % 2U) == 0U;
    input.samples[kTargetSourceSample - phase_tap] =
        coefficient_is_positive
            ? std::numeric_limits<std::int16_t>::max()
            : std::numeric_limits<std::int16_t>::min();
  }

  PlaybackPcmFrame48k output{};
  const auto result = renderer.Process(input, &output);
  BOOMPI_EXPECT(context, result.produced());
  BOOMPI_EXPECT(context, result.over_full_scale_samples > 0U);
  BOOMPI_EXPECT(context, result.limited_samples > 0U);
  BOOMPI_EXPECT(context, result.safety_saturated_samples == 0U);
  BOOMPI_EXPECT(context, output.HasValidLength());
  constexpr std::int16_t kPositiveCeiling = 31129;
  constexpr std::int16_t kNegativeCeiling = -31130;
  bool samples_are_bounded = true;
  for (std::size_t sample = 0U; sample < output.valid_samples; ++sample) {
    samples_are_bounded =
        samples_are_bounded &&
        output.samples[sample] <= kPositiveCeiling &&
        output.samples[sample] >= kNegativeCeiling;
  }
  BOOMPI_EXPECT(context, samples_are_bounded);
  BOOMPI_EXPECT(context,
                output.samples[kTargetSourceSample * 2U + 1U] > 0);
}

}  // namespace

int main() {
  boompi::test::TestContext context;
  TestConfigurationAndTransactionalArm(context);
  TestUnityGainLimiterAndMetadata(context);
  TestDuckAcrossFrames(context);
  TestNullAndStaleDoNotAdvance(context);
  TestFaultsRequireNewGeneration(context);
  TestExplicitEosDrain(context);
  TestFullFrameEosAndPendingDisarm(context);
  TestWideFirOvershootReachesLimiter(context);

  if (context.failures() != 0U) {
    std::cerr << context.failures()
              << " playback renderer expectation(s) failed\n";
    return 1;
  }
  std::cout << "boomPI playback renderer tests passed\n";
  return 0;
}
