#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <type_traits>
#include <utility>

#include "boompi/audio/audio_dsp_engine.h"
#include "boompi/audio/wake_word_engine.h"
#include "boompi/test/fake_audio_engines.h"
#include "boompi/test/test_context.h"

#define BOOMPI_EXPECT(context, expression) \
  (context).Expect((expression), #expression, __FILE__, __LINE__)

namespace {

using boompi::StatusCode;
using boompi::audio::AudioDspEngineConfig;
using boompi::audio::AudioDspFeature;
using boompi::audio::AudioDspFeatureMask;
using boompi::audio::AudioDspProcessCode;
using boompi::audio::AudioDspProcessResult;
using boompi::audio::AudioDspReferenceLayout;
using boompi::audio::AudioDspReferenceSource;
using boompi::audio::DspCaptureFrame16k;
using boompi::audio::Mono16kFrame;
using boompi::audio::SampleFormat;
using boompi::audio::WakeWordDecision;
using boompi::audio::WakeWordError;
using boompi::audio::WakeWordProcessResult;
using boompi::audio::WakeWordResetReason;
using boompi::audio::WakeWordResetResult;

static_assert(std::is_trivial<AudioDspProcessResult>::value,
              "audio DSP result must remain trivial");
static_assert(std::is_standard_layout<AudioDspProcessResult>::value,
              "audio DSP result must remain standard-layout");
static_assert(std::is_trivial<WakeWordProcessResult>::value,
              "wake-word process result must remain trivial");
static_assert(std::is_standard_layout<WakeWordProcessResult>::value,
              "wake-word process result must remain standard-layout");
static_assert(std::is_trivial<WakeWordResetResult>::value,
              "wake-word reset result must remain trivial");
static_assert(std::is_standard_layout<WakeWordResetResult>::value,
              "wake-word reset result must remain standard-layout");
constexpr WakeWordResetResult kSuccessfulReset{WakeWordError::kNone, true};
constexpr WakeWordResetResult kFailedReset{WakeWordError::kResetFailed,
                                            false};
constexpr WakeWordResetResult kInconsistentResetSuccess{
    WakeWordError::kBackendException, true};
constexpr WakeWordResetResult kInconsistentResetFailure{
    WakeWordError::kNone, false};
static_assert(kSuccessfulReset.valid() && kFailedReset.valid(),
              "reset success and failure payloads must be consistent");
static_assert(!kInconsistentResetSuccess.valid() &&
                  !kInconsistentResetFailure.valid(),
              "inconsistent reset payloads must be rejected");
static_assert(
    noexcept(std::declval<boompi::audio::AudioDspEngine&>().Process(
        std::declval<const DspCaptureFrame16k&>(),
        std::declval<Mono16kFrame*>())),
    "AudioDspEngine::Process must remain noexcept");
static_assert(
    noexcept(std::declval<boompi::audio::WakeWordEngine&>().Process(
        std::declval<const Mono16kFrame&>())),
    "WakeWordEngine::Process must remain noexcept");
static_assert(
    noexcept(std::declval<boompi::audio::WakeWordEngine&>().Reset(
        std::declval<WakeWordResetReason>())),
    "WakeWordEngine::Reset must remain noexcept");

AudioDspEngineConfig ValidDspConfig(
    const AudioDspReferenceSource reference_source =
        AudioDspReferenceSource::kHardwareCapture,
    const AudioDspReferenceLayout reference_layout =
        AudioDspReferenceLayout::kStereo) {
  return {reference_source, reference_layout,
          boompi::audio::kBoompiV1AudioDspFeatures};
}

DspCaptureFrame16k MakeDspInput(const std::uint32_t epoch,
                                const std::uint32_t stream_id,
                                const std::uint32_t sequence = 0U,
                                const std::uint32_t turn_id = 42U) {
  DspCaptureFrame16k input{};
  input.metadata.monotonic_timestamp_us =
      1000U + static_cast<std::uint64_t>(sequence) * 20000U;
  input.metadata.sequence = sequence;
  input.metadata.stream_id = stream_id;
  input.metadata.turn_id = turn_id;
  input.metadata.epoch = epoch;
  for (std::size_t channel = 0U; channel < input.planes.size(); ++channel) {
    for (std::size_t sample = 0U; sample < input.planes[channel].size();
         ++sample) {
      const auto value = static_cast<std::int32_t>(
                             (channel * 4001U + sample * 37U) % 30001U) -
                         15000;
      input.planes[channel][sample] = static_cast<std::int16_t>(value);
    }
  }
  return input;
}

Mono16kFrame MakeMonoInput(const std::uint32_t epoch = 1U,
                           const std::uint32_t stream_id = 2U,
                           const std::uint32_t sequence = 0U) {
  Mono16kFrame frame{};
  frame.format = {16000U, 20U, 1U, SampleFormat::kPcmS16Le};
  frame.metadata.monotonic_timestamp_us =
      1000U + static_cast<std::uint64_t>(sequence) * 20000U;
  frame.metadata.sequence = sequence;
  frame.metadata.stream_id = stream_id;
  frame.metadata.turn_id = 0U;
  frame.metadata.epoch = epoch;
  frame.samples_per_channel = 320U;
  for (std::size_t sample = 0U; sample < frame.samples.size(); ++sample) {
    frame.samples[sample] = static_cast<std::int16_t>(
        static_cast<std::int32_t>((sample * 53U) % 20001U) - 10000);
  }
  return frame;
}

void FillOutputSentinel(Mono16kFrame* const output,
                        const std::int16_t pcm_sentinel) {
  output->format = {16000U, 20U, 1U, SampleFormat::kPcmS16Le};
  output->metadata.monotonic_timestamp_us = 0x1122334455667788ULL;
  output->metadata.sequence = 101U;
  output->metadata.stream_id = 102U;
  output->metadata.turn_id = 103U;
  output->metadata.epoch = 104U;
  output->metadata.discontinuity = true;
  output->samples_per_channel = 320U;
  output->samples.fill(pcm_sentinel);
}

bool MetadataEqual(const boompi::audio::AudioFrameMetadata& lhs,
                   const boompi::audio::AudioFrameMetadata& rhs) {
  return lhs.monotonic_timestamp_us == rhs.monotonic_timestamp_us &&
         lhs.sequence == rhs.sequence && lhs.stream_id == rhs.stream_id &&
         lhs.turn_id == rhs.turn_id && lhs.epoch == rhs.epoch &&
         lhs.discontinuity == rhs.discontinuity;
}

void ExpectInvalidHeaderPreservingPcm(
    boompi::test::TestContext& context, const Mono16kFrame& output,
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
  BOOMPI_EXPECT(context, output.samples_per_channel == 0U);
  BOOMPI_EXPECT(context, output.samples.front() == pcm_sentinel);
  BOOMPI_EXPECT(context, output.samples.back() == pcm_sentinel);
}

void ExpectWakeFlags(boompi::test::TestContext& context,
                     const WakeWordProcessResult& result,
                     const bool valid, const bool produced,
                     const bool detected) {
  BOOMPI_EXPECT(context, result.valid() == valid);
  BOOMPI_EXPECT(context, result.produced() == produced);
  BOOMPI_EXPECT(context, result.detected() == detected);
}

void TestAudioDspConfigValidation(boompi::test::TestContext& context) {
  AudioDspEngineConfig config{};
  auto status = boompi::audio::ValidateAudioDspEngineConfig(config);
  BOOMPI_EXPECT(context, !status.ok());
  BOOMPI_EXPECT(context, status.code() == StatusCode::kInvalidArgument);

  config = ValidDspConfig();
  config.reference_source =
      static_cast<AudioDspReferenceSource>(0xFFU);
  status = boompi::audio::ValidateAudioDspEngineConfig(config);
  BOOMPI_EXPECT(context, !status.ok());
  BOOMPI_EXPECT(context, status.code() == StatusCode::kInvalidArgument);

  config = ValidDspConfig();
  config.reference_layout =
      static_cast<AudioDspReferenceLayout>(0xFFU);
  status = boompi::audio::ValidateAudioDspEngineConfig(config);
  BOOMPI_EXPECT(context, !status.ok());
  BOOMPI_EXPECT(context, status.code() == StatusCode::kInvalidArgument);

  config = ValidDspConfig();
  config.requested_features = static_cast<AudioDspFeatureMask>(
      config.requested_features | static_cast<AudioDspFeatureMask>(0x80U));
  status = boompi::audio::ValidateAudioDspEngineConfig(config);
  BOOMPI_EXPECT(context, !status.ok());
  BOOMPI_EXPECT(context, status.code() == StatusCode::kInvalidArgument);

  constexpr std::array<AudioDspFeature, 4U> kRequiredFeatures{{
      AudioDspFeature::kEchoCancellation,
      AudioDspFeature::kNoiseSuppression,
      AudioDspFeature::kBeamforming,
      AudioDspFeature::kAutomaticGainControl,
  }};
  for (const AudioDspFeature missing_feature : kRequiredFeatures) {
    config = ValidDspConfig();
    config.requested_features = static_cast<AudioDspFeatureMask>(
        config.requested_features ^ boompi::audio::ToMask(missing_feature));
    status = boompi::audio::ValidateAudioDspEngineConfig(config);
    BOOMPI_EXPECT(context, !status.ok());
    BOOMPI_EXPECT(context, status.code() == StatusCode::kInvalidArgument);
  }

  constexpr std::array<AudioDspReferenceSource, 2U> kSources{{
      AudioDspReferenceSource::kHardwareCapture,
      AudioDspReferenceSource::kSoftwarePlayback,
  }};
  constexpr std::array<AudioDspReferenceLayout, 3U> kLayouts{{
      AudioDspReferenceLayout::kMonoLeft,
      AudioDspReferenceLayout::kMonoRight,
      AudioDspReferenceLayout::kStereo,
  }};
  for (const AudioDspReferenceSource source : kSources) {
    for (const AudioDspReferenceLayout layout : kLayouts) {
      status = boompi::audio::ValidateAudioDspEngineConfig(
          ValidDspConfig(source, layout));
      BOOMPI_EXPECT(context, status.ok());
    }
  }
}

void TestUnavailableAudioDspEngine(
    boompi::test::TestContext& context) {
  boompi::audio::UnavailableAudioDspEngine engine;
  auto status = engine.Configure(AudioDspEngineConfig{});
  BOOMPI_EXPECT(context, !status.ok());
  BOOMPI_EXPECT(context, status.code() == StatusCode::kNotSupported);
  status = engine.Configure(ValidDspConfig());
  BOOMPI_EXPECT(context, !status.ok());
  BOOMPI_EXPECT(context, status.code() == StatusCode::kNotSupported);
  status = engine.Arm(0U, 0U);
  BOOMPI_EXPECT(context, !status.ok());
  BOOMPI_EXPECT(context, status.code() == StatusCode::kNotSupported);
  status = engine.Arm(1U, 2U);
  BOOMPI_EXPECT(context, !status.ok());
  BOOMPI_EXPECT(context, status.code() == StatusCode::kNotSupported);
  engine.Disarm();
  engine.Disarm();

  const auto input = MakeDspInput(1U, 2U);
  const auto null_result = engine.Process(input, nullptr);
  BOOMPI_EXPECT(context,
                null_result.code == AudioDspProcessCode::kInvalidOutput);
  BOOMPI_EXPECT(context, !null_result.produced());
  BOOMPI_EXPECT(context, !null_result.requires_new_generation());

  constexpr std::int16_t kSentinel = 22222;
  Mono16kFrame output{};
  FillOutputSentinel(&output, kSentinel);
  const auto result = engine.Process(input, &output);
  BOOMPI_EXPECT(context,
                result.code == AudioDspProcessCode::kBackendUnavailable);
  BOOMPI_EXPECT(context, !result.produced());
  BOOMPI_EXPECT(context, !result.requires_new_generation());
  ExpectInvalidHeaderPreservingPcm(context, output, kSentinel);
  BOOMPI_EXPECT(context, output.samples[0U] != input.planes[0U][0U]);
}

void TestFakeAudioDspLifecycle(boompi::test::TestContext& context) {
  boompi::test::FakeAudioDspEngine engine;
  auto input = MakeDspInput(10U, 20U, 7U);
  constexpr std::int16_t kSentinel = 24567;
  Mono16kFrame output{};

  FillOutputSentinel(&output, kSentinel);
  auto result = engine.Process(input, &output);
  BOOMPI_EXPECT(context,
                result.code == AudioDspProcessCode::kNotConfigured);
  ExpectInvalidHeaderPreservingPcm(context, output, kSentinel);
  BOOMPI_EXPECT(context, engine.backend_process_count() == 0U);

  auto status = engine.Arm(10U, 20U);
  BOOMPI_EXPECT(context, !status.ok());
  BOOMPI_EXPECT(context, status.code() == StatusCode::kFailedPrecondition);
  BOOMPI_EXPECT(context, engine.arm_count() == 1U);

  status = engine.Configure(AudioDspEngineConfig{});
  BOOMPI_EXPECT(context, !status.ok());
  BOOMPI_EXPECT(context, status.code() == StatusCode::kInvalidArgument);
  status = engine.Configure(ValidDspConfig());
  BOOMPI_EXPECT(context, status.ok());
  BOOMPI_EXPECT(context, engine.configured());
  status = engine.Configure(ValidDspConfig());
  BOOMPI_EXPECT(context, !status.ok());
  BOOMPI_EXPECT(context, status.code() == StatusCode::kFailedPrecondition);
  BOOMPI_EXPECT(context, engine.configure_count() == 3U);

  FillOutputSentinel(&output, kSentinel);
  result = engine.Process(input, &output);
  BOOMPI_EXPECT(context, result.code == AudioDspProcessCode::kNotArmed);
  ExpectInvalidHeaderPreservingPcm(context, output, kSentinel);

  status = engine.Arm(0U, 20U);
  BOOMPI_EXPECT(context, !status.ok());
  BOOMPI_EXPECT(context, status.code() == StatusCode::kInvalidArgument);
  status = engine.Arm(10U, 0U);
  BOOMPI_EXPECT(context, !status.ok());
  BOOMPI_EXPECT(context, status.code() == StatusCode::kInvalidArgument);
  status = engine.Arm(10U, 20U);
  BOOMPI_EXPECT(context, status.ok());
  BOOMPI_EXPECT(context, engine.armed());
  BOOMPI_EXPECT(context, engine.active_epoch() == 10U);
  BOOMPI_EXPECT(context, engine.active_stream_id() == 20U);
  BOOMPI_EXPECT(context, engine.reset_count() == 1U);
  status = engine.Arm(10U, 20U);
  BOOMPI_EXPECT(context, !status.ok());
  BOOMPI_EXPECT(context, status.code() == StatusCode::kFailedPrecondition);
  BOOMPI_EXPECT(context, engine.active_epoch() == 10U);
  BOOMPI_EXPECT(context, engine.active_stream_id() == 20U);
  BOOMPI_EXPECT(context, engine.arm_count() == 5U);

  auto stale_epoch = MakeDspInput(9U, 20U, 900U);
  FillOutputSentinel(&output, kSentinel);
  result = engine.Process(stale_epoch, &output);
  BOOMPI_EXPECT(context,
                result.code == AudioDspProcessCode::kEpochMismatch);
  ExpectInvalidHeaderPreservingPcm(context, output, kSentinel);
  BOOMPI_EXPECT(context, !engine.faulted());
  BOOMPI_EXPECT(context, engine.backend_process_count() == 0U);

  auto stale_stream = MakeDspInput(10U, 19U, 901U);
  result = engine.Process(stale_stream, &output);
  BOOMPI_EXPECT(context,
                result.code == AudioDspProcessCode::kStreamMismatch);
  BOOMPI_EXPECT(context, !engine.faulted());
  BOOMPI_EXPECT(context, engine.active_epoch() == 10U);
  BOOMPI_EXPECT(context, engine.active_stream_id() == 20U);
  BOOMPI_EXPECT(context, engine.backend_process_count() == 0U);

  result = engine.Process(input, &output);
  BOOMPI_EXPECT(context, result.code == AudioDspProcessCode::kProduced);
  BOOMPI_EXPECT(context, result.produced());
  BOOMPI_EXPECT(context, !result.requires_new_generation());
  BOOMPI_EXPECT(context, output.HasValidLength());
  BOOMPI_EXPECT(context, MetadataEqual(output.metadata, input.metadata));
  BOOMPI_EXPECT(context, output.samples == input.planes[0U]);
  BOOMPI_EXPECT(context, output.samples != input.planes[1U]);
  BOOMPI_EXPECT(context, engine.backend_process_count() == 1U);

  engine.Disarm();
  BOOMPI_EXPECT(context, !engine.armed());
  BOOMPI_EXPECT(context, !engine.faulted());
  BOOMPI_EXPECT(context, engine.active_epoch() == 0U);
  BOOMPI_EXPECT(context, engine.active_stream_id() == 0U);
  BOOMPI_EXPECT(context, engine.disarm_count() == 1U);
  BOOMPI_EXPECT(context, engine.reset_count() == 2U);
  result = engine.Process(input, &output);
  BOOMPI_EXPECT(context, result.code == AudioDspProcessCode::kNotArmed);
  status = engine.Arm(10U, 20U);
  BOOMPI_EXPECT(context, !status.ok());
  status = engine.Arm(11U, 20U);
  BOOMPI_EXPECT(context, status.ok());
  BOOMPI_EXPECT(context, engine.reset_count() == 3U);
  status = engine.Arm(10U, 99U);
  BOOMPI_EXPECT(context, !status.ok());
  BOOMPI_EXPECT(context, status.code() == StatusCode::kFailedPrecondition);
  status = engine.Arm(11U, 99U);
  BOOMPI_EXPECT(context, !status.ok());
  BOOMPI_EXPECT(context, status.code() == StatusCode::kFailedPrecondition);
  BOOMPI_EXPECT(context, engine.active_epoch() == 11U);
  BOOMPI_EXPECT(context, engine.active_stream_id() == 20U);
  BOOMPI_EXPECT(context, engine.reset_count() == 3U);

  auto discontinuity = MakeDspInput(11U, 20U, 8U);
  discontinuity.metadata.discontinuity = true;
  FillOutputSentinel(&output, kSentinel);
  result = engine.Process(discontinuity, &output);
  BOOMPI_EXPECT(context,
                result.code == AudioDspProcessCode::kDiscontinuity);
  BOOMPI_EXPECT(context, result.requires_new_generation());
  BOOMPI_EXPECT(context, engine.faulted());
  BOOMPI_EXPECT(context, engine.backend_process_count() == 1U);
  ExpectInvalidHeaderPreservingPcm(context, output, kSentinel);

  auto current = MakeDspInput(11U, 20U, 9U);
  result = engine.Process(current, &output);
  BOOMPI_EXPECT(context, result.code == AudioDspProcessCode::kFaulted);
  BOOMPI_EXPECT(context, result.requires_new_generation());
  BOOMPI_EXPECT(context, engine.backend_process_count() == 1U);
  stale_epoch = MakeDspInput(10U, 20U, 902U);
  result = engine.Process(stale_epoch, &output);
  BOOMPI_EXPECT(context,
                result.code == AudioDspProcessCode::kEpochMismatch);
  BOOMPI_EXPECT(context, engine.faulted());

  status = engine.Arm(12U, 21U);
  BOOMPI_EXPECT(context, status.ok());
  BOOMPI_EXPECT(context, !engine.faulted());
  BOOMPI_EXPECT(context, engine.reset_count() == 4U);
  current = MakeDspInput(12U, 21U, 0U, 77U);
  result = engine.Process(current, &output);
  BOOMPI_EXPECT(context, result.code == AudioDspProcessCode::kProduced);
  BOOMPI_EXPECT(context, MetadataEqual(output.metadata, current.metadata));
  BOOMPI_EXPECT(context, output.samples == current.planes[0U]);
  BOOMPI_EXPECT(context, engine.backend_process_count() == 2U);

  engine.FailNextProcess();
  result = engine.Process(current, nullptr);
  BOOMPI_EXPECT(context, result.code == AudioDspProcessCode::kInvalidOutput);
  BOOMPI_EXPECT(context, engine.backend_process_count() == 2U);
  BOOMPI_EXPECT(context, !engine.faulted());

  FillOutputSentinel(&output, kSentinel);
  result = engine.Process(current, &output);
  BOOMPI_EXPECT(context,
                result.code == AudioDspProcessCode::kBackendFailure);
  BOOMPI_EXPECT(context, result.requires_new_generation());
  BOOMPI_EXPECT(context, engine.backend_process_count() == 3U);
  BOOMPI_EXPECT(context, engine.faulted());
  ExpectInvalidHeaderPreservingPcm(context, output, kSentinel);
  result = engine.Process(current, &output);
  BOOMPI_EXPECT(context, result.code == AudioDspProcessCode::kFaulted);
  BOOMPI_EXPECT(context, engine.backend_process_count() == 3U);

  status = engine.Arm(13U, 22U);
  BOOMPI_EXPECT(context, status.ok());
  BOOMPI_EXPECT(context, !engine.faulted());
  BOOMPI_EXPECT(context, engine.reset_count() == 5U);
  current = MakeDspInput(13U, 22U, 500U);
  result = engine.Process(current, &output);
  BOOMPI_EXPECT(context, result.code == AudioDspProcessCode::kProduced);
  BOOMPI_EXPECT(context, engine.backend_process_count() == 4U);

  engine.Disarm();
  BOOMPI_EXPECT(context, engine.disarm_count() == 2U);
  BOOMPI_EXPECT(context, engine.reset_count() == 6U);
  engine.Disarm();
  BOOMPI_EXPECT(context, engine.disarm_count() == 3U);
  BOOMPI_EXPECT(context, engine.reset_count() == 6U);
  BOOMPI_EXPECT(context, engine.configure_count() == 3U);
  BOOMPI_EXPECT(context, engine.arm_count() == 11U);
}

void TestWakeWordResultMatrix(boompi::test::TestContext& context) {
  constexpr WakeWordProcessResult kNoEvent{
      WakeWordDecision::kNoEvent, WakeWordError::kNone, 0U, 0U, false};
  constexpr WakeWordProcessResult kSilence{
      WakeWordDecision::kSilence, WakeWordError::kNone, 0U, 0U, false};
  constexpr WakeWordProcessResult kDetectedWithoutScore{
      WakeWordDecision::kDetected, WakeWordError::kNone, 1U, 0U, false};
  constexpr WakeWordProcessResult kDetectedAtMinimumScore{
      WakeWordDecision::kDetected, WakeWordError::kNone, 1U, 0U, true};
  constexpr WakeWordProcessResult kDetectedAtMaximumScore{
      WakeWordDecision::kDetected, WakeWordError::kNone, 2U, 1000U, true};
  constexpr WakeWordProcessResult kInvalidFrame{
      WakeWordDecision::kInvalidFrame, WakeWordError::kInvalidFormat,
      0U, 0U, false};
  constexpr WakeWordProcessResult kUnavailable{
      WakeWordDecision::kBackendUnavailable,
      WakeWordError::kBackendUnavailable, 0U, 0U, false};

  ExpectWakeFlags(context, kNoEvent, true, true, false);
  ExpectWakeFlags(context, kSilence, true, true, false);
  ExpectWakeFlags(context, kDetectedWithoutScore, true, true, true);
  ExpectWakeFlags(context, kDetectedAtMinimumScore, true, true, true);
  ExpectWakeFlags(context, kDetectedAtMaximumScore, true, true, true);
  ExpectWakeFlags(context, kInvalidFrame, true, false, false);
  ExpectWakeFlags(context, kUnavailable, true, false, false);

  constexpr std::array<WakeWordError, 5U> kBackendFailures{{
      WakeWordError::kNotInitialized,
      WakeWordError::kBackendRejectedAudio,
      WakeWordError::kBackendException,
      WakeWordError::kUnexpectedKeywordIndex,
      WakeWordError::kResetFailed,
  }};
  for (const WakeWordError error : kBackendFailures) {
    ExpectWakeFlags(
        context,
        {WakeWordDecision::kBackendError, error, 0U, 0U, false},
        true, false, false);
    ExpectWakeFlags(context,
                    {WakeWordDecision::kFaulted, error, 0U, 0U, false},
                    true, false, false);
  }

  ExpectWakeFlags(
      context,
      {WakeWordDecision::kDetected, WakeWordError::kNone, 1U, 1001U, true},
      false, false, false);
  ExpectWakeFlags(
      context,
      {WakeWordDecision::kDetected, WakeWordError::kNone, 1U, 1U, false},
      false, false, false);
  ExpectWakeFlags(
      context,
      {WakeWordDecision::kNoEvent, WakeWordError::kNone, 0U, 0U, true},
      false, false, false);
  ExpectWakeFlags(
      context,
      {WakeWordDecision::kNoEvent, WakeWordError::kNone, 1U, 0U, false},
      false, false, false);
  ExpectWakeFlags(
      context,
      {WakeWordDecision::kSilence, WakeWordError::kBackendException,
       0U, 0U, false},
      false, false, false);
  ExpectWakeFlags(
      context,
      {WakeWordDecision::kDetected, WakeWordError::kNone, 0U, 0U, false},
      false, false, false);
  ExpectWakeFlags(
      context,
      {WakeWordDecision::kDetected, WakeWordError::kBackendException,
       1U, 0U, false},
      false, false, false);
  ExpectWakeFlags(
      context,
      {WakeWordDecision::kInvalidFrame, WakeWordError::kNone,
       0U, 0U, false},
      false, false, false);
  ExpectWakeFlags(
      context,
      {WakeWordDecision::kBackendUnavailable, WakeWordError::kNone,
       0U, 0U, false},
      false, false, false);
  ExpectWakeFlags(
      context,
      {WakeWordDecision::kBackendError, WakeWordError::kInvalidFormat,
       0U, 0U, false},
      false, false, false);
  ExpectWakeFlags(
      context,
      {static_cast<WakeWordDecision>(0xFFU), WakeWordError::kNone,
       0U, 0U, false},
      false, false, false);
}

void TestUnavailableWakeWordEngine(
    boompi::test::TestContext& context) {
  boompi::audio::UnavailableWakeWordEngine engine;
  Mono16kFrame invalid{};
  auto result = engine.Process(invalid);
  BOOMPI_EXPECT(context,
                result.decision == WakeWordDecision::kInvalidFrame);
  BOOMPI_EXPECT(context, result.error == WakeWordError::kInvalidFormat);
  ExpectWakeFlags(context, result, true, false, false);

  auto valid = MakeMonoInput();
  BOOMPI_EXPECT(context, valid.metadata.turn_id == 0U);
  BOOMPI_EXPECT(context, valid.HasValidLength());
  result = engine.Process(valid);
  BOOMPI_EXPECT(context,
                result.decision == WakeWordDecision::kBackendUnavailable);
  BOOMPI_EXPECT(context,
                result.error == WakeWordError::kBackendUnavailable);
  ExpectWakeFlags(context, result, true, false, false);

  valid.metadata.epoch = 0U;
  result = engine.Process(valid);
  BOOMPI_EXPECT(context,
                result.decision == WakeWordDecision::kInvalidFrame);
  valid = MakeMonoInput();
  valid.samples_per_channel = 319U;
  result = engine.Process(valid);
  BOOMPI_EXPECT(context,
                result.decision == WakeWordDecision::kInvalidFrame);

  for (const WakeWordResetReason reason :
       {WakeWordResetReason::kGenerationActivated,
        WakeWordResetReason::kVadSegmentEnded,
        WakeWordResetReason::kDiscontinuity}) {
    const auto reset = engine.Reset(reason);
    BOOMPI_EXPECT(context, reset.valid());
    BOOMPI_EXPECT(context, !reset.reset);
    BOOMPI_EXPECT(context,
                  reset.error == WakeWordError::kBackendUnavailable);
  }
}

void TestFakeWakeWordEngine(boompi::test::TestContext& context) {
  boompi::test::FakeWakeWordEngine engine;
  const auto valid = MakeMonoInput();
  auto result = engine.Process(valid);
  BOOMPI_EXPECT(context, result.decision == WakeWordDecision::kNoEvent);
  ExpectWakeFlags(context, result, true, true, false);
  BOOMPI_EXPECT(context, engine.process_count() == 1U);

  const WakeWordProcessResult no_event{
      WakeWordDecision::kNoEvent, WakeWordError::kNone, 0U, 0U, false};
  const WakeWordProcessResult silence{
      WakeWordDecision::kSilence, WakeWordError::kNone, 0U, 0U, false};
  const WakeWordProcessResult detected_without_score{
      WakeWordDecision::kDetected, WakeWordError::kNone, 1U, 0U, false};
  const WakeWordProcessResult detected_with_score{
      WakeWordDecision::kDetected, WakeWordError::kNone, 2U, 875U, true};
  const WakeWordProcessResult backend_error{
      WakeWordDecision::kBackendError, WakeWordError::kBackendException,
      0U, 0U, false};
  const WakeWordProcessResult faulted{
      WakeWordDecision::kFaulted, WakeWordError::kBackendRejectedAudio,
      0U, 0U, false};

  BOOMPI_EXPECT(context, engine.SetNextResult(no_event));
  BOOMPI_EXPECT(context, !engine.SetNextResult(silence));
  BOOMPI_EXPECT(context, engine.scripted_result_pending());
  result = engine.Process(valid);
  BOOMPI_EXPECT(context, result.decision == WakeWordDecision::kNoEvent);
  BOOMPI_EXPECT(context, !engine.scripted_result_pending());

  BOOMPI_EXPECT(context, engine.SetNextResult(silence));
  result = engine.Process(valid);
  BOOMPI_EXPECT(context, result.decision == WakeWordDecision::kSilence);
  ExpectWakeFlags(context, result, true, true, false);

  BOOMPI_EXPECT(context, engine.SetNextResult(detected_without_score));
  result = engine.Process(valid);
  BOOMPI_EXPECT(context, result.keyword_index == 1U);
  BOOMPI_EXPECT(context, !result.score_available);
  ExpectWakeFlags(context, result, true, true, true);

  BOOMPI_EXPECT(context, engine.SetNextResult(detected_with_score));
  result = engine.Process(valid);
  BOOMPI_EXPECT(context, result.keyword_index == 2U);
  BOOMPI_EXPECT(context, result.score_milli == 875U);
  BOOMPI_EXPECT(context, result.score_available);
  ExpectWakeFlags(context, result, true, true, true);

  BOOMPI_EXPECT(context, engine.SetNextResult(backend_error));
  result = engine.Process(valid);
  BOOMPI_EXPECT(context, result.decision == WakeWordDecision::kBackendError);
  BOOMPI_EXPECT(context, result.error == WakeWordError::kBackendException);
  ExpectWakeFlags(context, result, true, false, false);

  BOOMPI_EXPECT(context, engine.SetNextResult(faulted));
  auto invalid = valid;
  invalid.samples_per_channel = 319U;
  result = engine.Process(invalid);
  BOOMPI_EXPECT(context,
                result.decision == WakeWordDecision::kInvalidFrame);
  BOOMPI_EXPECT(context, engine.scripted_result_pending());
  result = engine.Process(valid);
  BOOMPI_EXPECT(context, result.decision == WakeWordDecision::kFaulted);
  BOOMPI_EXPECT(context, !engine.scripted_result_pending());
  ExpectWakeFlags(context, result, true, false, false);

  const WakeWordProcessResult invalid_script{
      WakeWordDecision::kDetected, WakeWordError::kNone, 0U, 0U, false};
  BOOMPI_EXPECT(context, !engine.SetNextResult(invalid_script));
  BOOMPI_EXPECT(context, !engine.scripted_result_pending());
  result = engine.Process(valid);
  BOOMPI_EXPECT(context, result.decision == WakeWordDecision::kNoEvent);
  BOOMPI_EXPECT(context, engine.process_count() == 9U);

  BOOMPI_EXPECT(context, !engine.has_last_reset_reason());
  auto reset = engine.Reset(WakeWordResetReason::kGenerationActivated);
  BOOMPI_EXPECT(context, reset.valid());
  BOOMPI_EXPECT(context, reset.reset);
  BOOMPI_EXPECT(context, reset.error == WakeWordError::kNone);
  BOOMPI_EXPECT(context, engine.has_last_reset_reason());
  BOOMPI_EXPECT(
      context,
      engine.last_reset_reason() ==
          WakeWordResetReason::kGenerationActivated);

  BOOMPI_EXPECT(context,
                !engine.SetNextResetError(WakeWordError::kNone));
  BOOMPI_EXPECT(
      context,
      !engine.SetNextResetError(static_cast<WakeWordError>(0xFFU)));
  BOOMPI_EXPECT(context,
                engine.SetNextResetError(WakeWordError::kResetFailed));
  BOOMPI_EXPECT(context,
                !engine.SetNextResetError(WakeWordError::kBackendException));
  BOOMPI_EXPECT(context, engine.reset_error_pending());
  reset = engine.Reset(WakeWordResetReason::kDiscontinuity);
  BOOMPI_EXPECT(context, reset.valid());
  BOOMPI_EXPECT(context, !reset.reset);
  BOOMPI_EXPECT(context, reset.error == WakeWordError::kResetFailed);
  BOOMPI_EXPECT(context, !engine.reset_error_pending());
  BOOMPI_EXPECT(
      context,
      engine.last_reset_reason() == WakeWordResetReason::kDiscontinuity);

  reset = engine.Reset(WakeWordResetReason::kVadSegmentEnded);
  BOOMPI_EXPECT(context, reset.valid());
  BOOMPI_EXPECT(context, reset.reset);
  BOOMPI_EXPECT(context, reset.error == WakeWordError::kNone);
  BOOMPI_EXPECT(
      context,
      engine.last_reset_reason() == WakeWordResetReason::kVadSegmentEnded);
  BOOMPI_EXPECT(context, engine.reset_count() == 3U);
}

}  // namespace

int main() {
  boompi::test::TestContext context;
  TestAudioDspConfigValidation(context);
  TestUnavailableAudioDspEngine(context);
  TestFakeAudioDspLifecycle(context);
  TestWakeWordResultMatrix(context);
  TestUnavailableWakeWordEngine(context);
  TestFakeWakeWordEngine(context);

  if (context.failures() != 0U) {
    std::cerr << context.failures()
              << " audio backend contract expectation(s) failed\n";
    return 1;
  }
  std::cout << "boomPI audio backend contract tests passed\n";
  return 0;
}
