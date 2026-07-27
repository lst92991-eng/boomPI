#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <type_traits>
#include <utility>

#include "boompi/audio/audio_frame.h"
#include "boompi/audio/wake_word_engine.h"
#include "boompi/platform/rv1106/snowboy_wake_word_engine.h"
#include "boompi/test/test_context.h"
#include "snowboy_legacy_bridge_fake.h"

#define BOOMPI_EXPECT(context, expression) \
  (context).Expect((expression), #expression, __FILE__, __LINE__)

namespace {

using boompi::audio::Mono16kFrame;
using boompi::audio::SampleFormat;
using boompi::audio::WakeWordDecision;
using boompi::audio::WakeWordError;
using boompi::audio::WakeWordResetReason;
using boompi::platform::rv1106::SnowboyWakeWordConfig;
using boompi::platform::rv1106::SnowboyWakeWordCreateError;
using boompi::platform::rv1106::SnowboyWakeWordCreateResult;
using boompi::platform::rv1106::SnowboyWakeWordEngine;
using boompi::test::ResetSnowboyLegacyBridgeFake;
using boompi::test::SnowboyLegacyBridgeFake;

static_assert(
    noexcept(std::declval<SnowboyWakeWordEngine&>().Process(
        std::declval<const Mono16kFrame&>())),
    "Snowboy Process must remain noexcept");
static_assert(
    noexcept(std::declval<SnowboyWakeWordEngine&>().Reset(
        std::declval<WakeWordResetReason>())),
    "Snowboy Reset must remain noexcept");
static_assert(std::is_standard_layout<SnowboyWakeWordCreateResult>::value,
              "create result must remain standard-layout");

SnowboyWakeWordConfig ValidConfig() {
  SnowboyWakeWordConfig config{};
  config.resource_path = "fixture-common.res";
  config.model_path = "fixture-model.umdl";
  config.sensitivity = "0.5";
  config.audio_gain = 1.0F;
  config.apply_frontend = false;
  return config;
}

Mono16kFrame ValidFrame() {
  Mono16kFrame frame{};
  frame.format = {16000U, 20U, 1U, SampleFormat::kPcmS16Le};
  frame.metadata.monotonic_timestamp_us = 1000000U;
  frame.metadata.sequence = 7U;
  frame.metadata.stream_id = 9U;
  frame.metadata.turn_id = 0U;
  frame.metadata.epoch = 3U;
  frame.samples_per_channel = 320U;
  for (std::size_t sample = 0U; sample < frame.samples.size(); ++sample) {
    frame.samples[sample] = static_cast<std::int16_t>(
        static_cast<std::int32_t>(sample) - 160);
  }
  return frame;
}

void TestConfigValidation(boompi::test::TestContext& context) {
  ResetSnowboyLegacyBridgeFake();
  SnowboyWakeWordCreateResult result{};
  auto config = ValidConfig();
  config.resource_path.clear();
  auto engine = SnowboyWakeWordEngine::Create(config, &result);
  BOOMPI_EXPECT(context, engine == nullptr);
  BOOMPI_EXPECT(context,
                result.error == SnowboyWakeWordCreateError::kInvalidConfig);
  BOOMPI_EXPECT(context, SnowboyLegacyBridgeFake().create_calls == 0U);

  config = ValidConfig();
  config.audio_gain = std::numeric_limits<float>::infinity();
  engine = SnowboyWakeWordEngine::Create(config, &result);
  BOOMPI_EXPECT(context, engine == nullptr);
  BOOMPI_EXPECT(context,
                result.error == SnowboyWakeWordCreateError::kInvalidConfig);
  BOOMPI_EXPECT(context, SnowboyLegacyBridgeFake().create_calls == 0U);
}

void TestCreateFailures(boompi::test::TestContext& context) {
  ResetSnowboyLegacyBridgeFake();
  auto& state = SnowboyLegacyBridgeFake();
  SnowboyWakeWordCreateResult result{};

  state.create_status = BOOMPI_SNOWBOY_LEGACY_BACKEND_EXCEPTION;
  auto engine =
      SnowboyWakeWordEngine::Create(ValidConfig(), &result);
  BOOMPI_EXPECT(context, engine == nullptr);
  BOOMPI_EXPECT(
      context,
      result.error == SnowboyWakeWordCreateError::kBackendException);
  BOOMPI_EXPECT(context, state.destroy_calls == 0U);

  ResetSnowboyLegacyBridgeFake();
  SnowboyLegacyBridgeFake().info.sample_rate_hz = 8000U;
  engine = SnowboyWakeWordEngine::Create(ValidConfig(), &result);
  BOOMPI_EXPECT(context, engine == nullptr);
  BOOMPI_EXPECT(
      context,
      result.error ==
          SnowboyWakeWordCreateError::kUnsupportedAudioFormat);
  BOOMPI_EXPECT(context,
                SnowboyLegacyBridgeFake().destroy_calls == 1U);

  ResetSnowboyLegacyBridgeFake();
  SnowboyLegacyBridgeFake().info.keyword_count = 0U;
  engine = SnowboyWakeWordEngine::Create(ValidConfig(), &result);
  BOOMPI_EXPECT(context, engine == nullptr);
  BOOMPI_EXPECT(
      context,
      result.error == SnowboyWakeWordCreateError::kInvalidKeywordCount);
  BOOMPI_EXPECT(context,
                SnowboyLegacyBridgeFake().destroy_calls == 1U);
}

void TestSuccessfulMappings(boompi::test::TestContext& context) {
  ResetSnowboyLegacyBridgeFake();
  auto& state = SnowboyLegacyBridgeFake();
  state.info.keyword_count = 2U;
  SnowboyWakeWordCreateResult create{};
  auto engine = SnowboyWakeWordEngine::Create(ValidConfig(), &create);
  BOOMPI_EXPECT(context, engine != nullptr);
  BOOMPI_EXPECT(context, create.ok());
  BOOMPI_EXPECT(context, create.sample_rate_hz == 16000U);
  BOOMPI_EXPECT(context, create.channels == 1U);
  BOOMPI_EXPECT(context, create.bits_per_sample == 16U);
  BOOMPI_EXPECT(context, create.keyword_count == 2U);
  BOOMPI_EXPECT(context, engine->keyword_count() == 2U);

  auto frame = ValidFrame();
  state.process_result = -2;
  auto process = engine->Process(frame);
  BOOMPI_EXPECT(context, process.valid());
  BOOMPI_EXPECT(context, process.decision == WakeWordDecision::kSilence);
  BOOMPI_EXPECT(context, process.error == WakeWordError::kNone);

  state.process_result = 0;
  process = engine->Process(frame);
  BOOMPI_EXPECT(context, process.valid());
  BOOMPI_EXPECT(context, process.decision == WakeWordDecision::kNoEvent);

  state.process_result = 2;
  process = engine->Process(frame);
  BOOMPI_EXPECT(context, process.valid());
  BOOMPI_EXPECT(context, process.detected());
  BOOMPI_EXPECT(context, process.keyword_index == 2U);
  BOOMPI_EXPECT(context, !process.score_available);
  BOOMPI_EXPECT(context, process.score_milli == 0U);
  BOOMPI_EXPECT(context, state.last_sample_count == 320U);
  BOOMPI_EXPECT(context, state.first_sample == -160);
  BOOMPI_EXPECT(context, state.last_sample == 159);

  frame.samples_per_channel = 319U;
  const std::uint32_t process_calls_before = state.process_calls;
  process = engine->Process(frame);
  BOOMPI_EXPECT(context, process.valid());
  BOOMPI_EXPECT(
      context, process.decision == WakeWordDecision::kInvalidFrame);
  BOOMPI_EXPECT(context, process.error == WakeWordError::kInvalidFormat);
  BOOMPI_EXPECT(context, state.process_calls == process_calls_before);
}

void TestFaultLatchAndReset(boompi::test::TestContext& context) {
  ResetSnowboyLegacyBridgeFake();
  auto& state = SnowboyLegacyBridgeFake();
  SnowboyWakeWordCreateResult create{};
  auto engine = SnowboyWakeWordEngine::Create(ValidConfig(), &create);
  BOOMPI_EXPECT(context, engine != nullptr);
  auto frame = ValidFrame();

  state.process_result = -1;
  auto process = engine->Process(frame);
  BOOMPI_EXPECT(context, process.valid());
  BOOMPI_EXPECT(
      context, process.decision == WakeWordDecision::kBackendError);
  BOOMPI_EXPECT(
      context, process.error == WakeWordError::kBackendRejectedAudio);

  state.process_result = 0;
  const std::uint32_t process_calls_before = state.process_calls;
  process = engine->Process(frame);
  BOOMPI_EXPECT(context, process.valid());
  BOOMPI_EXPECT(context, process.decision == WakeWordDecision::kFaulted);
  BOOMPI_EXPECT(
      context, process.error == WakeWordError::kBackendRejectedAudio);
  BOOMPI_EXPECT(context, state.process_calls == process_calls_before);

  auto reset = engine->Reset(WakeWordResetReason::kGenerationActivated);
  BOOMPI_EXPECT(context, reset.valid());
  BOOMPI_EXPECT(context, reset.reset);
  BOOMPI_EXPECT(context, reset.error == WakeWordError::kNone);
  process = engine->Process(frame);
  BOOMPI_EXPECT(context, process.decision == WakeWordDecision::kNoEvent);

  state.process_result = -3;
  process = engine->Process(frame);
  BOOMPI_EXPECT(
      context, process.decision == WakeWordDecision::kBackendError);
  BOOMPI_EXPECT(
      context, process.error == WakeWordError::kBackendRejectedAudio);
  reset = engine->Reset(WakeWordResetReason::kGenerationActivated);
  BOOMPI_EXPECT(context, reset.valid());
  BOOMPI_EXPECT(context, reset.reset);

  state.process_result = 2;
  process = engine->Process(frame);
  BOOMPI_EXPECT(
      context, process.error == WakeWordError::kUnexpectedKeywordIndex);

  state.reset_status = BOOMPI_SNOWBOY_LEGACY_BACKEND_REJECTED;
  state.reset_result = 0;
  reset = engine->Reset(WakeWordResetReason::kDiscontinuity);
  BOOMPI_EXPECT(context, reset.valid());
  BOOMPI_EXPECT(context, !reset.reset);
  BOOMPI_EXPECT(context, reset.error == WakeWordError::kResetFailed);
}

void TestBridgeExceptionMapping(boompi::test::TestContext& context) {
  ResetSnowboyLegacyBridgeFake();
  auto& state = SnowboyLegacyBridgeFake();
  SnowboyWakeWordCreateResult create{};
  auto engine = SnowboyWakeWordEngine::Create(ValidConfig(), &create);
  BOOMPI_EXPECT(context, engine != nullptr);

  state.process_status = BOOMPI_SNOWBOY_LEGACY_BACKEND_EXCEPTION;
  const auto process = engine->Process(ValidFrame());
  BOOMPI_EXPECT(context, process.valid());
  BOOMPI_EXPECT(
      context, process.decision == WakeWordDecision::kBackendError);
  BOOMPI_EXPECT(
      context, process.error == WakeWordError::kBackendException);

  state.reset_status = BOOMPI_SNOWBOY_LEGACY_BACKEND_EXCEPTION;
  state.reset_result = 0;
  const auto reset =
      engine->Reset(WakeWordResetReason::kVadSegmentEnded);
  BOOMPI_EXPECT(context, reset.valid());
  BOOMPI_EXPECT(context, !reset.reset);
  BOOMPI_EXPECT(context, reset.error == WakeWordError::kBackendException);
}

}  // namespace

int main() {
  boompi::test::TestContext context;
  TestConfigValidation(context);
  TestCreateFailures(context);
  TestSuccessfulMappings(context);
  TestFaultLatchAndReset(context);
  TestBridgeExceptionMapping(context);

  if (context.failures() != 0U) {
    std::cerr << context.failures()
              << " Snowboy adapter expectation(s) failed\n";
    return 1;
  }
  std::cout << "boomPI Snowboy adapter tests passed\n";
  return 0;
}
