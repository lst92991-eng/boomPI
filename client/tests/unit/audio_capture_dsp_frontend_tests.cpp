#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>

#include "boompi/audio/capture_dsp_frontend.h"
#include "boompi/test/test_context.h"

#define BOOMPI_EXPECT(context, expression) \
  (context).Expect((expression), #expression, __FILE__, __LINE__)

namespace {

boompi::audio::ChannelMap IdentityMap() {
  boompi::audio::ChannelMap channel_map{};
  channel_map[boompi::audio::CaptureRole::kMicLeft] = {0U, 1};
  channel_map[boompi::audio::CaptureRole::kMicRight] = {1U, 1};
  channel_map[boompi::audio::CaptureRole::kReferenceLeft] = {2U, 1};
  channel_map[boompi::audio::CaptureRole::kReferenceRight] = {3U, 1};
  return channel_map;
}

boompi::audio::ChannelMap PermutedMap() {
  boompi::audio::ChannelMap channel_map{};
  channel_map[boompi::audio::CaptureRole::kMicLeft] = {2U, 1};
  channel_map[boompi::audio::CaptureRole::kMicRight] = {0U, -1};
  channel_map[boompi::audio::CaptureRole::kReferenceLeft] = {3U, 1};
  channel_map[boompi::audio::CaptureRole::kReferenceRight] = {1U, -1};
  return channel_map;
}

boompi::audio::CaptureFrame MakeFrame(
    const std::uint32_t epoch, const std::uint32_t stream_id,
    const std::uint32_t sequence, const std::uint64_t timestamp_us,
    const std::uint32_t turn_id = 0U, const std::uint8_t channels = 4U) {
  boompi::audio::CaptureFrame frame{};
  frame.format = {48000U, 20U, channels,
                  boompi::audio::SampleFormat::kPcmS16Le};
  frame.metadata.monotonic_timestamp_us = timestamp_us;
  frame.metadata.sequence = sequence;
  frame.metadata.stream_id = stream_id;
  frame.metadata.turn_id = turn_id;
  frame.metadata.epoch = epoch;
  frame.samples_per_channel = 960U;
  return frame;
}

void FillDeterministic(boompi::audio::CaptureFrame* const frame,
                       const std::uint32_t seed) {
  const auto channels = static_cast<std::size_t>(frame->format.channels);
  for (std::size_t sample = 0U;
       sample < boompi::audio::kSamplesPer48k20ms; ++sample) {
    for (std::size_t slot = 0U; slot < channels; ++slot) {
      const auto value = static_cast<std::int32_t>(
                             (sample * 73U + slot * 1901U + seed * 131U) %
                             30001U) -
                         15000;
      frame->samples[sample * channels + slot] =
          static_cast<std::int16_t>(value);
    }
  }
}

bool MetadataEqual(const boompi::audio::AudioFrameMetadata& lhs,
                   const boompi::audio::AudioFrameMetadata& rhs) {
  return lhs.monotonic_timestamp_us == rhs.monotonic_timestamp_us &&
         lhs.sequence == rhs.sequence && lhs.stream_id == rhs.stream_id &&
         lhs.turn_id == rhs.turn_id && lhs.epoch == rhs.epoch &&
         lhs.discontinuity == rhs.discontinuity;
}

bool OutputEqual(const boompi::audio::DspCaptureFrame16k& lhs,
                 const boompi::audio::DspCaptureFrame16k& rhs) {
  return MetadataEqual(lhs.metadata, rhs.metadata) && lhs.planes == rhs.planes;
}

void FillOutputSentinel(boompi::audio::DspCaptureFrame16k* const output) {
  output->metadata.monotonic_timestamp_us = 0x1122334455667788ULL;
  output->metadata.sequence = 0xA1A2A3A4U;
  output->metadata.stream_id = 0xB1B2B3B4U;
  output->metadata.turn_id = 0xC1C2C3C4U;
  output->metadata.epoch = 0xD1D2D3D4U;
  output->metadata.discontinuity = true;
  for (std::size_t channel = 0U; channel < output->planes.size(); ++channel) {
    output->planes[channel].fill(
        static_cast<std::int16_t>(1234 + static_cast<std::int32_t>(channel)));
  }
}

void ExpectOutputUnchanged(
    boompi::test::TestContext& context,
    const boompi::audio::DspCaptureFrame16k& output,
    const boompi::audio::DspCaptureFrame16k& before) {
  BOOMPI_EXPECT(context, OutputEqual(output, before));
}

void TestCreateValidationAndPreservation(
    boompi::test::TestContext& context) {
  const auto identity_map = IdentityMap();
  BOOMPI_EXPECT(
      context,
      !boompi::audio::CaptureDspFrontend::Create(identity_map, nullptr).ok());

  boompi::audio::CaptureDspFrontend unconfigured;
  BOOMPI_EXPECT(context, !unconfigured.Arm(1U, 1U).ok());
  auto frame = MakeFrame(1U, 1U, 0U, 1000U);
  FillDeterministic(&frame, 1U);
  boompi::audio::DspCaptureFrame16k output{};
  FillOutputSentinel(&output);
  const auto before = output;
  auto result = unconfigured.Process(frame, &output);
  BOOMPI_EXPECT(context,
                result.code ==
                    boompi::audio::CaptureDspFrontendCode::kNotConfigured);
  BOOMPI_EXPECT(context,
                result.continuity ==
                    boompi::audio::FrameContinuityResult::kNotArmed);
  BOOMPI_EXPECT(context, !result.produced());
  BOOMPI_EXPECT(context, !result.requires_new_generation());
  ExpectOutputUnchanged(context, output, before);

  auto invalid_map = identity_map;
  invalid_map[boompi::audio::CaptureRole::kMicRight].input_slot = 0U;
  BOOMPI_EXPECT(
      context,
      !boompi::audio::CaptureDspFrontend::Create(invalid_map, &unconfigured)
           .ok());
  BOOMPI_EXPECT(context, !unconfigured.Arm(1U, 1U).ok());
  BOOMPI_EXPECT(
      context,
      boompi::audio::CaptureDspFrontend::Create(PermutedMap(), &unconfigured)
          .ok());

  BOOMPI_EXPECT(
      context,
      !boompi::audio::CaptureDspFrontend::Create(identity_map, &unconfigured)
           .ok());
  BOOMPI_EXPECT(context, unconfigured.Arm(1U, 1U).ok());

  boompi::audio::CaptureChannelMapper mapper;
  BOOMPI_EXPECT(context,
                boompi::audio::CaptureChannelMapper::Create(PermutedMap(),
                                                             &mapper)
                    .ok());
  boompi::audio::FirDecimator48To16 fir;
  boompi::audio::CapturePlanes48k mapped{};
  boompi::audio::CapturePlanes16k expected_planes{};
  BOOMPI_EXPECT(context, mapper.Process(frame, &mapped).ok());
  BOOMPI_EXPECT(context, fir.Process(mapped, &expected_planes).ok());
  result = unconfigured.Process(frame, &output);
  BOOMPI_EXPECT(context,
                result.code ==
                    boompi::audio::CaptureDspFrontendCode::kProduced);
  BOOMPI_EXPECT(context, output.planes == expected_planes);
}

void TestArmDisarmAndGenerationRules(
    boompi::test::TestContext& context) {
  boompi::audio::CaptureDspFrontend frontend;
  BOOMPI_EXPECT(
      context,
      boompi::audio::CaptureDspFrontend::Create(IdentityMap(), &frontend)
          .ok());
  auto frame = MakeFrame(1U, 7U, 10U, 1000U);
  FillDeterministic(&frame, 2U);
  boompi::audio::DspCaptureFrame16k output{};
  FillOutputSentinel(&output);
  const auto before = output;

  auto result = frontend.Process(frame, &output);
  BOOMPI_EXPECT(context,
                result.code ==
                    boompi::audio::CaptureDspFrontendCode::kNotArmed);
  ExpectOutputUnchanged(context, output, before);
  BOOMPI_EXPECT(context, !frontend.Arm(0U, 7U).ok());
  BOOMPI_EXPECT(context, !frontend.Arm(1U, 0U).ok());
  result = frontend.Process(frame, &output);
  BOOMPI_EXPECT(context,
                result.code ==
                    boompi::audio::CaptureDspFrontendCode::kNotArmed);

  BOOMPI_EXPECT(context, frontend.Arm(1U, 7U).ok());
  BOOMPI_EXPECT(context, !frontend.Arm(0U, 7U).ok());
  BOOMPI_EXPECT(context, !frontend.Arm(1U, 7U).ok());
  result = frontend.Process(frame, &output);
  BOOMPI_EXPECT(context,
                result.code ==
                    boompi::audio::CaptureDspFrontendCode::kProduced);
  BOOMPI_EXPECT(context,
                result.continuity ==
                    boompi::audio::FrameContinuityResult::kAcceptedFirst);
  BOOMPI_EXPECT(context, result.produced());
  BOOMPI_EXPECT(context, !result.requires_new_generation());

  frontend.Disarm();
  FillOutputSentinel(&output);
  const auto disarmed_before = output;
  result = frontend.Process(frame, &output);
  BOOMPI_EXPECT(context,
                result.code ==
                    boompi::audio::CaptureDspFrontendCode::kNotArmed);
  ExpectOutputUnchanged(context, output, disarmed_before);
  BOOMPI_EXPECT(context, !frontend.Arm(1U, 7U).ok());
  BOOMPI_EXPECT(context, frontend.Arm(2U, 7U).ok());
  frame.metadata.epoch = 2U;
  frame.metadata.sequence = 90U;
  frame.metadata.monotonic_timestamp_us = 9000U;
  BOOMPI_EXPECT(context,
                frontend.Process(frame, &output).code ==
                    boompi::audio::CaptureDspFrontendCode::kProduced);
}

void TestBitExactPrimitivePipelineAndHistory(
    boompi::test::TestContext& context) {
  const auto channel_map = PermutedMap();
  boompi::audio::CaptureDspFrontend frontend;
  boompi::audio::CaptureChannelMapper mapper;
  boompi::audio::FirDecimator48To16 fir;
  BOOMPI_EXPECT(
      context,
      boompi::audio::CaptureDspFrontend::Create(channel_map, &frontend).ok());
  BOOMPI_EXPECT(context,
                boompi::audio::CaptureChannelMapper::Create(channel_map,
                                                             &mapper)
                    .ok());
  BOOMPI_EXPECT(context, frontend.Arm(5U, 9U).ok());

  boompi::audio::CapturePlanes16k continuous_second{};
  boompi::audio::CapturePlanes48k second_mapped{};
  for (std::uint32_t frame_index = 0U; frame_index < 2U; ++frame_index) {
    auto input = MakeFrame(5U, 9U, 30U + frame_index,
                           10000U + frame_index * 20000U,
                           100U + frame_index);
    FillDeterministic(&input, 20U + frame_index);
    boompi::audio::CapturePlanes48k mapped{};
    boompi::audio::CapturePlanes16k expected{};
    const auto mapper_result = mapper.Process(input, &mapped);
    const auto fir_result = fir.Process(mapped, &expected);
    boompi::audio::DspCaptureFrame16k actual{};
    const auto frontend_result = frontend.Process(input, &actual);

    BOOMPI_EXPECT(context, mapper_result.ok());
    BOOMPI_EXPECT(context, fir_result.ok());
    BOOMPI_EXPECT(context,
                  frontend_result.code ==
                      boompi::audio::CaptureDspFrontendCode::kProduced);
    BOOMPI_EXPECT(context,
                  frontend_result.continuity ==
                      (frame_index == 0U
                           ? boompi::audio::FrameContinuityResult::
                                 kAcceptedFirst
                           : boompi::audio::FrameContinuityResult::
                                 kAcceptedContinuous));
    BOOMPI_EXPECT(context,
                  frontend_result.mapper_saturated_samples ==
                      mapper_result.saturated_samples);
    BOOMPI_EXPECT(context,
                  frontend_result.fir_saturated_samples ==
                      fir_result.saturated_samples);
    BOOMPI_EXPECT(context, actual.planes == expected);
    BOOMPI_EXPECT(context, MetadataEqual(actual.metadata, input.metadata));
    if (frame_index == 1U) {
      continuous_second = expected;
      second_mapped = mapped;
    }
  }

  boompi::audio::FirDecimator48To16 reset_fir;
  boompi::audio::CapturePlanes16k reset_second{};
  BOOMPI_EXPECT(context, reset_fir.Process(second_mapped, &reset_second).ok());
  BOOMPI_EXPECT(context, continuous_second != reset_second);

  static_assert(boompi::audio::DspCaptureFrame16k::kSampleRateHz == 16000U,
                "frontend DSP rate changed");
  static_assert(boompi::audio::DspCaptureFrame16k::kFrameDurationMs == 20U,
                "frontend DSP frame duration changed");
  static_assert(boompi::audio::DspCaptureFrame16k::kChannelCount == 4U,
                "frontend DSP channel count changed");
  static_assert(
      boompi::audio::DspCaptureFrame16k::kSamplesPerChannel == 320U,
      "frontend DSP frame size changed");
}

void TestStaleFramesAndNewGenerationReset(
    boompi::test::TestContext& context) {
  boompi::audio::CaptureDspFrontend candidate;
  boompi::audio::CaptureDspFrontend control;
  BOOMPI_EXPECT(
      context,
      boompi::audio::CaptureDspFrontend::Create(IdentityMap(), &candidate)
          .ok());
  BOOMPI_EXPECT(
      context,
      boompi::audio::CaptureDspFrontend::Create(IdentityMap(), &control).ok());
  BOOMPI_EXPECT(context, candidate.Arm(10U, 20U).ok());
  BOOMPI_EXPECT(context, control.Arm(10U, 20U).ok());

  auto prelude = MakeFrame(10U, 20U, 0U, 1000U);
  FillDeterministic(&prelude, 31U);
  boompi::audio::DspCaptureFrame16k candidate_output{};
  boompi::audio::DspCaptureFrame16k control_output{};
  BOOMPI_EXPECT(context,
                candidate.Process(prelude, &candidate_output).code ==
                    boompi::audio::CaptureDspFrontendCode::kProduced);
  BOOMPI_EXPECT(context,
                control.Process(prelude, &control_output).code ==
                    boompi::audio::CaptureDspFrontendCode::kProduced);

  auto stale_epoch = MakeFrame(9U, 20U, 900U, 2U);
  FillDeterministic(&stale_epoch, 32U);
  FillOutputSentinel(&candidate_output);
  auto before = candidate_output;
  auto result = candidate.Process(stale_epoch, &candidate_output);
  BOOMPI_EXPECT(context,
                result.code ==
                    boompi::audio::CaptureDspFrontendCode::kEpochMismatch);
  BOOMPI_EXPECT(context,
                result.continuity ==
                    boompi::audio::FrameContinuityResult::kEpochMismatch);
  BOOMPI_EXPECT(context, !result.produced());
  BOOMPI_EXPECT(context, !result.requires_new_generation());
  ExpectOutputUnchanged(context, candidate_output, before);

  auto stale_stream = MakeFrame(10U, 19U, 901U, 3U);
  FillDeterministic(&stale_stream, 33U);
  result = candidate.Process(stale_stream, &candidate_output);
  BOOMPI_EXPECT(context,
                result.code ==
                    boompi::audio::CaptureDspFrontendCode::kStreamMismatch);
  ExpectOutputUnchanged(context, candidate_output, before);

  auto next = MakeFrame(10U, 20U, 1U, 21000U, 77U);
  FillDeterministic(&next, 34U);
  result = candidate.Process(next, &candidate_output);
  const auto control_result = control.Process(next, &control_output);
  BOOMPI_EXPECT(context,
                result.code ==
                    boompi::audio::CaptureDspFrontendCode::kProduced);
  BOOMPI_EXPECT(context,
                control_result.code ==
                    boompi::audio::CaptureDspFrontendCode::kProduced);
  BOOMPI_EXPECT(context, OutputEqual(candidate_output, control_output));

  boompi::audio::CaptureDspFrontend fresh_generation;
  BOOMPI_EXPECT(
      context,
      boompi::audio::CaptureDspFrontend::Create(IdentityMap(),
                                                 &fresh_generation)
          .ok());
  BOOMPI_EXPECT(context, candidate.Arm(11U, 20U).ok());
  BOOMPI_EXPECT(context, fresh_generation.Arm(11U, 20U).ok());
  auto generation_first = MakeFrame(11U, 20U, 500U, 50000U, 88U);
  FillDeterministic(&generation_first, 35U);
  result = candidate.Process(generation_first, &candidate_output);
  const auto fresh_result =
      fresh_generation.Process(generation_first, &control_output);
  BOOMPI_EXPECT(context,
                result.code ==
                    boompi::audio::CaptureDspFrontendCode::kProduced);
  BOOMPI_EXPECT(context,
                fresh_result.code ==
                    boompi::audio::CaptureDspFrontendCode::kProduced);
  BOOMPI_EXPECT(context, OutputEqual(candidate_output, control_output));
}

enum class ContinuityFaultCase : std::uint8_t {
  kDiscontinuity,
  kGap,
  kDuplicate,
  kWrap,
  kTimestampRegression,
};

void ExerciseContinuityFault(boompi::test::TestContext& context,
                             const ContinuityFaultCase fault_case) {
  boompi::audio::CaptureDspFrontend frontend;
  BOOMPI_EXPECT(
      context,
      boompi::audio::CaptureDspFrontend::Create(IdentityMap(), &frontend)
          .ok());
  BOOMPI_EXPECT(context, frontend.Arm(1U, 1U).ok());

  std::uint32_t first_sequence = 10U;
  if (fault_case == ContinuityFaultCase::kWrap) {
    first_sequence = std::numeric_limits<std::uint32_t>::max();
  }
  auto first = MakeFrame(1U, 1U, first_sequence, 1000U);
  FillDeterministic(&first, 40U);
  boompi::audio::DspCaptureFrame16k output{};
  BOOMPI_EXPECT(context,
                frontend.Process(first, &output).code ==
                    boompi::audio::CaptureDspFrontendCode::kProduced);

  auto fault = MakeFrame(1U, 1U, first_sequence + 1U, 2000U);
  auto expected_continuity =
      boompi::audio::FrameContinuityResult::kSequenceBreak;
  switch (fault_case) {
    case ContinuityFaultCase::kDiscontinuity:
      fault.metadata.discontinuity = true;
      expected_continuity =
          boompi::audio::FrameContinuityResult::kDiscontinuity;
      break;
    case ContinuityFaultCase::kGap:
      fault.metadata.sequence = first_sequence + 2U;
      break;
    case ContinuityFaultCase::kDuplicate:
      fault.metadata.sequence = first_sequence;
      break;
    case ContinuityFaultCase::kWrap:
      fault.metadata.sequence = 0U;
      break;
    case ContinuityFaultCase::kTimestampRegression:
      fault.metadata.monotonic_timestamp_us = 999U;
      expected_continuity =
          boompi::audio::FrameContinuityResult::kTimestampNotIncreasing;
      break;
  }
  FillDeterministic(&fault, 41U);
  FillOutputSentinel(&output);
  const auto before = output;
  auto result = frontend.Process(fault, &output);
  BOOMPI_EXPECT(context,
                result.code ==
                    boompi::audio::CaptureDspFrontendCode::kContinuityFault);
  BOOMPI_EXPECT(context, result.continuity == expected_continuity);
  BOOMPI_EXPECT(context, !result.produced());
  BOOMPI_EXPECT(context, result.requires_new_generation());
  BOOMPI_EXPECT(context, result.mapper_saturated_samples == 0U);
  BOOMPI_EXPECT(context, result.fir_saturated_samples == 0U);
  ExpectOutputUnchanged(context, output, before);

  auto suppressed = MakeFrame(1U, 1U, first_sequence + 1U, 3000U);
  FillDeterministic(&suppressed, 42U);
  result = frontend.Process(suppressed, &output);
  BOOMPI_EXPECT(context,
                result.code ==
                    boompi::audio::CaptureDspFrontendCode::kFaulted);
  BOOMPI_EXPECT(context,
                result.continuity ==
                    boompi::audio::FrameContinuityResult::kFaulted);
  BOOMPI_EXPECT(context, !result.produced());
  BOOMPI_EXPECT(context, result.requires_new_generation());
  ExpectOutputUnchanged(context, output, before);
  BOOMPI_EXPECT(context, !frontend.Arm(1U, 1U).ok());
  result = frontend.Process(suppressed, &output);
  BOOMPI_EXPECT(context,
                result.code ==
                    boompi::audio::CaptureDspFrontendCode::kFaulted);

  BOOMPI_EXPECT(context, frontend.Arm(2U, 1U).ok());
  auto recovered = MakeFrame(2U, 1U, 77U, 7000U, 55U);
  FillDeterministic(&recovered, 43U);
  result = frontend.Process(recovered, &output);
  BOOMPI_EXPECT(context,
                result.code ==
                    boompi::audio::CaptureDspFrontendCode::kProduced);
  BOOMPI_EXPECT(context,
                result.continuity ==
                    boompi::audio::FrameContinuityResult::kAcceptedFirst);
}

void TestContinuityFaultLatchingAndRecovery(
    boompi::test::TestContext& context) {
  ExerciseContinuityFault(context, ContinuityFaultCase::kDiscontinuity);
  ExerciseContinuityFault(context, ContinuityFaultCase::kGap);
  ExerciseContinuityFault(context, ContinuityFaultCase::kDuplicate);
  ExerciseContinuityFault(context, ContinuityFaultCase::kWrap);
  ExerciseContinuityFault(context, ContinuityFaultCase::kTimestampRegression);
}

void TestTransformFaultLatchingAndRecovery(
    boompi::test::TestContext& context) {
  boompi::audio::CaptureDspFrontend frontend;
  BOOMPI_EXPECT(
      context,
      boompi::audio::CaptureDspFrontend::Create(IdentityMap(), &frontend)
          .ok());
  BOOMPI_EXPECT(context, frontend.Arm(3U, 4U).ok());

  auto two_channel = MakeFrame(3U, 4U, 0U, 1000U, 0U, 2U);
  FillDeterministic(&two_channel, 50U);
  BOOMPI_EXPECT(context, two_channel.HasValidLength());
  boompi::audio::DspCaptureFrame16k output{};
  FillOutputSentinel(&output);
  const auto before = output;
  auto result = frontend.Process(two_channel, &output);
  BOOMPI_EXPECT(context,
                result.code ==
                    boompi::audio::CaptureDspFrontendCode::kTransformFault);
  BOOMPI_EXPECT(context,
                result.continuity ==
                    boompi::audio::FrameContinuityResult::kAcceptedFirst);
  BOOMPI_EXPECT(context, !result.produced());
  BOOMPI_EXPECT(context, result.requires_new_generation());
  ExpectOutputUnchanged(context, output, before);

  auto stale_epoch = MakeFrame(2U, 4U, 900U, 2U);
  FillDeterministic(&stale_epoch, 52U);
  result = frontend.Process(stale_epoch, &output);
  BOOMPI_EXPECT(context,
                result.code ==
                    boompi::audio::CaptureDspFrontendCode::kEpochMismatch);
  BOOMPI_EXPECT(context, !result.produced());
  BOOMPI_EXPECT(context, !result.requires_new_generation());
  ExpectOutputUnchanged(context, output, before);

  auto stale_stream = MakeFrame(3U, 5U, 901U, 3U);
  FillDeterministic(&stale_stream, 53U);
  result = frontend.Process(stale_stream, &output);
  BOOMPI_EXPECT(context,
                result.code ==
                    boompi::audio::CaptureDspFrontendCode::kStreamMismatch);
  BOOMPI_EXPECT(context, !result.produced());
  BOOMPI_EXPECT(context, !result.requires_new_generation());
  ExpectOutputUnchanged(context, output, before);

  auto valid = MakeFrame(3U, 4U, 1U, 21000U);
  FillDeterministic(&valid, 51U);
  result = frontend.Process(valid, &output);
  BOOMPI_EXPECT(context,
                result.code ==
                    boompi::audio::CaptureDspFrontendCode::kFaulted);
  BOOMPI_EXPECT(context,
                result.continuity ==
                    boompi::audio::FrameContinuityResult::kAcceptedContinuous);
  BOOMPI_EXPECT(context, !result.produced());
  BOOMPI_EXPECT(context, result.requires_new_generation());
  ExpectOutputUnchanged(context, output, before);
  BOOMPI_EXPECT(context, !frontend.Arm(3U, 4U).ok());

  BOOMPI_EXPECT(context, frontend.Arm(4U, 4U).ok());
  valid.metadata.epoch = 4U;
  valid.metadata.sequence = 100U;
  valid.metadata.monotonic_timestamp_us = 50000U;
  result = frontend.Process(valid, &output);
  BOOMPI_EXPECT(context,
                result.code ==
                    boompi::audio::CaptureDspFrontendCode::kProduced);
}

void TestNullOutputCanRetrySameFrame(
    boompi::test::TestContext& context) {
  boompi::audio::CaptureDspFrontend frontend;
  BOOMPI_EXPECT(
      context,
      boompi::audio::CaptureDspFrontend::Create(IdentityMap(), &frontend)
          .ok());
  BOOMPI_EXPECT(context, frontend.Arm(6U, 8U).ok());
  auto frame = MakeFrame(6U, 8U, 42U, 42000U, 99U);
  FillDeterministic(&frame, 60U);

  const auto invalid = frontend.Process(frame, nullptr);
  BOOMPI_EXPECT(context,
                invalid.code ==
                    boompi::audio::CaptureDspFrontendCode::kInvalidOutput);
  BOOMPI_EXPECT(context,
                invalid.continuity ==
                    boompi::audio::FrameContinuityResult::kNotArmed);
  BOOMPI_EXPECT(context, !invalid.produced());
  BOOMPI_EXPECT(context, !invalid.requires_new_generation());
  BOOMPI_EXPECT(context, invalid.mapper_saturated_samples == 0U);
  BOOMPI_EXPECT(context, invalid.fir_saturated_samples == 0U);

  boompi::audio::DspCaptureFrame16k output{};
  const auto retried = frontend.Process(frame, &output);
  BOOMPI_EXPECT(context,
                retried.code ==
                    boompi::audio::CaptureDspFrontendCode::kProduced);
  BOOMPI_EXPECT(context,
                retried.continuity ==
                    boompi::audio::FrameContinuityResult::kAcceptedFirst);
  BOOMPI_EXPECT(context, retried.produced());
  BOOMPI_EXPECT(context, !retried.requires_new_generation());
  BOOMPI_EXPECT(context, MetadataEqual(output.metadata, frame.metadata));
  BOOMPI_EXPECT(context, output.metadata.turn_id == 99U);
}

void TestMapperAndFirSaturationCounts(
    boompi::test::TestContext& context) {
  auto channel_map = IdentityMap();
  channel_map[boompi::audio::CaptureRole::kMicLeft].polarity = -1;
  boompi::audio::CaptureDspFrontend frontend;
  boompi::audio::CaptureChannelMapper mapper;
  boompi::audio::FirDecimator48To16 fir;
  BOOMPI_EXPECT(
      context,
      boompi::audio::CaptureDspFrontend::Create(channel_map, &frontend).ok());
  BOOMPI_EXPECT(context,
                boompi::audio::CaptureChannelMapper::Create(channel_map,
                                                             &mapper)
                    .ok());
  BOOMPI_EXPECT(context, frontend.Arm(7U, 9U).ok());

  auto frame = MakeFrame(7U, 9U, 0U, 1000U, 123U);
  for (std::size_t sample = 0U;
       sample < boompi::audio::kSamplesPer48k20ms; ++sample) {
    frame.samples[sample * 4U] =
        std::numeric_limits<std::int16_t>::min();
    for (std::size_t slot = 1U; slot < 4U; ++slot) {
      frame.samples[sample * 4U + slot] =
          std::numeric_limits<std::int16_t>::max();
    }
  }

  boompi::audio::CapturePlanes48k mapped{};
  boompi::audio::CapturePlanes16k expected{};
  const auto mapper_result = mapper.Process(frame, &mapped);
  const auto fir_result = fir.Process(mapped, &expected);
  boompi::audio::DspCaptureFrame16k actual{};
  const auto result = frontend.Process(frame, &actual);
  BOOMPI_EXPECT(context, mapper_result.ok());
  BOOMPI_EXPECT(context, mapper_result.saturated_samples == 960U);
  BOOMPI_EXPECT(context, fir_result.ok());
  BOOMPI_EXPECT(context, fir_result.saturated_samples > 0U);
  BOOMPI_EXPECT(context,
                result.code ==
                    boompi::audio::CaptureDspFrontendCode::kProduced);
  BOOMPI_EXPECT(context,
                result.mapper_saturated_samples ==
                    mapper_result.saturated_samples);
  BOOMPI_EXPECT(context,
                result.fir_saturated_samples == fir_result.saturated_samples);
  BOOMPI_EXPECT(context, actual.planes == expected);
  BOOMPI_EXPECT(context, MetadataEqual(actual.metadata, frame.metadata));
}

}  // namespace

int main() {
  boompi::test::TestContext context;
  TestCreateValidationAndPreservation(context);
  TestArmDisarmAndGenerationRules(context);
  TestBitExactPrimitivePipelineAndHistory(context);
  TestStaleFramesAndNewGenerationReset(context);
  TestContinuityFaultLatchingAndRecovery(context);
  TestTransformFaultLatchingAndRecovery(context);
  TestNullOutputCanRetrySameFrame(context);
  TestMapperAndFirSaturationCounts(context);

  if (context.failures() != 0U) {
    std::cerr << context.failures()
              << " capture DSP frontend expectation(s) failed\n";
    return 1;
  }
  std::cout << "boomPI capture DSP frontend tests passed\n";
  return 0;
}
