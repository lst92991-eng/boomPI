#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <type_traits>
#include <utility>

#include "boompi/audio/playback_worker.h"
#include "boompi/test/scripted_pcm_playback_sink.h"
#include "boompi/test/test_context.h"

#define BOOMPI_EXPECT(context, expression) \
  (context).Expect((expression), #expression, __FILE__, __LINE__)

namespace {

using boompi::audio::AcceptedRenderChunk48k;
using boompi::audio::AcceptedRenderQueue;
using boompi::audio::PcmPlaybackControlCode;
using boompi::audio::PcmPlaybackWriteCode;
using boompi::audio::PlaybackArmResultCode;
using boompi::audio::PlaybackCancelCode;
using boompi::audio::PlaybackCancelCommand;
using boompi::audio::PlaybackCriticalStreamEvent;
using boompi::audio::PlaybackCriticalStreamEventCode;
using boompi::audio::PlaybackCriticalStreamEventMailbox;
using boompi::audio::PlaybackDuckResultCode;
using boompi::audio::PlaybackEndOfStreamAcceptedEvent;
using boompi::audio::PlaybackEndOfStreamEventMailbox;
using boompi::audio::PlaybackEpochFence;
using boompi::audio::PlaybackGeneration;
using boompi::audio::PlaybackIngressQuiesceCode;
using boompi::audio::PlaybackLifecycleCommand;
using boompi::audio::PlaybackLifecycleCommandMailbox;
using boompi::audio::PlaybackLifecycleCommandType;
using boompi::audio::PlaybackLifecycleResult;
using boompi::audio::PlaybackLifecycleResultMailbox;
using boompi::audio::PlaybackLifecycleResultType;
using boompi::audio::PlaybackLocalCancelCode;
using boompi::audio::PlaybackLocalCancelCompletion;
using boompi::audio::PlaybackLocalCancelResultMailbox;
using boompi::audio::PlaybackReferenceResetCode;
using boompi::audio::PlaybackShutdownResultCode;
using boompi::audio::PlaybackUrgentCancelMailbox;
using boompi::audio::PlaybackWorkerConfig;
using boompi::audio::PlaybackWorkerCore;
using boompi::audio::PlaybackWorkerEndpoints;
using boompi::audio::PlaybackWorkerState;
using boompi::audio::PlaybackWorkerStepCode;
using boompi::audio::TtsIngressQueue;
using boompi::audio::TtsPcmFrame24k;
using boompi::test::ScriptedPcmPlaybackSink;

static_assert(std::is_trivially_copyable<PlaybackWorkerConfig>::value,
              "worker configuration must remain value-only");
static_assert(std::is_standard_layout<PlaybackWorkerConfig>::value,
              "worker configuration must remain standard-layout");
static_assert(noexcept(std::declval<PlaybackWorkerCore&>().Step()),
              "the worker hot step must remain noexcept");

PlaybackGeneration Generation(const std::uint32_t epoch,
                              const std::uint32_t turn_id,
                              const std::uint32_t stream_id) {
  return {epoch, turn_id, stream_id};
}

TtsPcmFrame24k MakeTtsFrame(
    const PlaybackGeneration& generation, const std::uint32_t sequence,
    const std::uint64_t timestamp_us, const std::int16_t sample_base = 1000,
    const std::uint16_t valid_source_samples = TtsPcmFrame24k::kFrameSamples,
    const bool end_of_stream = false) {
  TtsPcmFrame24k frame{};
  frame.format = {TtsPcmFrame24k::kSampleRateHz,
                  TtsPcmFrame24k::kFrameDurationMs,
                  TtsPcmFrame24k::kChannelCount, TtsPcmFrame24k::kSampleFormat};
  frame.metadata.monotonic_timestamp_us = timestamp_us;
  frame.metadata.sequence = sequence;
  frame.metadata.epoch = generation.epoch;
  frame.metadata.turn_id = generation.turn_id;
  frame.metadata.stream_id = generation.stream_id;
  frame.valid_source_samples = valid_source_samples;
  frame.end_of_stream = end_of_stream;
  for (std::size_t sample = 0U; sample < frame.samples.size(); ++sample) {
    frame.samples[sample] = sample < valid_source_samples
                                ? static_cast<std::int16_t>(
                                      static_cast<std::int32_t>(sample_base) +
                                      static_cast<std::int32_t>(sample % 101U))
                                : 0;
  }
  return frame;
}

bool PublishFrame(TtsIngressQueue* const queue, const TtsPcmFrame24k& frame) {
  auto write = queue->TryAcquireWrite();
  if (!write) {
    return false;
  }
  write.frame() = frame;
  return write.Publish();
}

bool PopAccepted(AcceptedRenderQueue* const queue,
                 AcceptedRenderChunk48k* const output) {
  auto read = queue->TryAcquireRead();
  if (!read || output == nullptr) {
    return false;
  }
  *output = read.frame();
  return read.Release();
}

PlaybackLifecycleCommand ArmCommand(const std::uint64_t request_id,
                                    const PlaybackGeneration& generation) {
  PlaybackLifecycleCommand command{};
  command.type = PlaybackLifecycleCommandType::kArm;
  command.request_id = request_id;
  command.generation = generation;
  return command;
}

PlaybackLifecycleCommand QuiesceCommand(
    const std::uint64_t request_id, const PlaybackGeneration& generation,
    const std::uint64_t retired_pcm_incarnation) {
  PlaybackLifecycleCommand command{};
  command.type = PlaybackLifecycleCommandType::kQuiesceIngress;
  command.request_id = request_id;
  command.generation = generation;
  command.retired_pcm_incarnation = retired_pcm_incarnation;
  command.value = true;
  return command;
}

PlaybackLifecycleCommand ConfirmReferenceCommand(
    const std::uint64_t request_id, const PlaybackGeneration& generation,
    const std::uint64_t retired_pcm_incarnation) {
  PlaybackLifecycleCommand command{};
  command.type = PlaybackLifecycleCommandType::kConfirmReferenceReset;
  command.request_id = request_id;
  command.generation = generation;
  command.retired_pcm_incarnation = retired_pcm_incarnation;
  return command;
}

PlaybackLifecycleCommand ShutdownCommand(const std::uint64_t request_id) {
  PlaybackLifecycleCommand command{};
  command.type = PlaybackLifecycleCommandType::kShutdown;
  command.request_id = request_id;
  return command;
}

PlaybackCancelCommand CancelCommand(const std::uint64_t request_id,
                                    const PlaybackGeneration& generation,
                                    const std::uint32_t cancel_epoch) {
  PlaybackCancelCommand command{};
  command.request_id = request_id;
  command.generation = generation;
  command.cancel_epoch = cancel_epoch;
  return command;
}

struct WorkerFixture final {
  PlaybackEpochFence fence{};
  ScriptedPcmPlaybackSink sink{};
  TtsIngressQueue ingress{};
  AcceptedRenderQueue accepted{};
  PlaybackUrgentCancelMailbox urgent_cancel{};
  PlaybackLifecycleCommandMailbox lifecycle_commands{};
  PlaybackLocalCancelResultMailbox local_cancel_results{};
  PlaybackLifecycleResultMailbox lifecycle_results{};
  PlaybackEndOfStreamEventMailbox eos_events{};
  PlaybackCriticalStreamEventMailbox critical_events{};
  PlaybackWorkerCore worker{};

  PlaybackWorkerEndpoints Endpoints() noexcept {
    return {&fence,
            &sink,
            &ingress,
            &accepted,
            &urgent_cancel,
            &lifecycle_commands,
            &local_cancel_results,
            &lifecycle_results,
            &eos_events,
            &critical_events};
  }

  bool CreateAndInitialize(boompi::test::TestContext& context,
                           const PlaybackWorkerConfig& config = {}) {
    const auto create_status =
        PlaybackWorkerCore::Create(config, Endpoints(), &worker);
    BOOMPI_EXPECT(context, create_status.ok());
    if (!create_status.ok()) {
      return false;
    }
    BOOMPI_EXPECT(context,
                  worker.state() == PlaybackWorkerState::kUninitialized);
    BOOMPI_EXPECT(context, sink.PushPrepareResult(
                               {PcmPlaybackControlCode::kSucceeded, 0}));
    const auto initialize_status = worker.Initialize();
    BOOMPI_EXPECT(context, initialize_status.ok());
    BOOMPI_EXPECT(context,
                  worker.state() == PlaybackWorkerState::kPreparedIdle);
    return initialize_status.ok();
  }
};

bool StepUntilLifecycleResult(WorkerFixture* const fixture,
                              PlaybackLifecycleResult* const output,
                              const std::uint32_t maximum_steps = 12U) {
  for (std::uint32_t step = 0U; step < maximum_steps; ++step) {
    fixture->worker.Step();
    if (fixture->lifecycle_results.TryPop(output)) {
      return true;
    }
  }
  return false;
}

bool StepUntilLocalCancel(WorkerFixture* const fixture,
                          PlaybackLocalCancelCompletion* const output,
                          const std::uint32_t maximum_steps = 12U) {
  for (std::uint32_t step = 0U; step < maximum_steps; ++step) {
    fixture->worker.Step();
    if (fixture->local_cancel_results.TryPop(output)) {
      return true;
    }
  }
  return false;
}

bool ArmWorker(boompi::test::TestContext& context, WorkerFixture* const fixture,
               const PlaybackGeneration& generation,
               const std::uint64_t request_id,
               const bool advance_fence = true) {
  if (advance_fence) {
    BOOMPI_EXPECT(context, fixture->fence.AdvanceTo(generation.epoch).ok());
  } else {
    BOOMPI_EXPECT(context, fixture->fence.Observe() == generation.epoch);
  }
  BOOMPI_EXPECT(context, fixture->lifecycle_commands.TryPush(
                             ArmCommand(request_id, generation)));
  const auto first_step = fixture->worker.Step();
  BOOMPI_EXPECT(context, first_step.code == PlaybackWorkerStepCode::kProgress);
  BOOMPI_EXPECT(context,
                fixture->worker.state() == PlaybackWorkerState::kArming);
  PlaybackLifecycleResult result{};
  BOOMPI_EXPECT(context, !fixture->lifecycle_results.TryPop(&result));
  if (!StepUntilLifecycleResult(fixture, &result)) {
    BOOMPI_EXPECT(context, false);
    return false;
  }
  BOOMPI_EXPECT(context, result.type == PlaybackLifecycleResultType::kArm);
  BOOMPI_EXPECT(context,
                result.arm.code == PlaybackArmResultCode::kAcknowledged);
  BOOMPI_EXPECT(context, result.arm.request_id == request_id);
  BOOMPI_EXPECT(context, result.arm.observed_fence_epoch == generation.epoch);
  BOOMPI_EXPECT(context,
                fixture->worker.state() == PlaybackWorkerState::kActive);
  return result.arm.acknowledged();
}

void ScriptSuccessfulCancel(boompi::test::TestContext& context,
                            ScriptedPcmPlaybackSink* const sink) {
  BOOMPI_EXPECT(context,
                sink->PushDropResult({PcmPlaybackControlCode::kSucceeded, 0}));
  BOOMPI_EXPECT(context, sink->PushPrepareResult(
                             {PcmPlaybackControlCode::kSucceeded, 0}));
}

void ExpectUncorrelatableLifecycleInputRejected(
    boompi::test::TestContext& context,
    const PlaybackLifecycleCommand& command) {
  WorkerFixture fixture;
  if (!fixture.CreateAndInitialize(context)) {
    return;
  }
  BOOMPI_EXPECT(context, fixture.lifecycle_commands.TryPush(command));
  const auto step = fixture.worker.Step();
  BOOMPI_EXPECT(context,
                step.code == PlaybackWorkerStepCode::kInvalidControlInput);
  BOOMPI_EXPECT(context,
                fixture.worker.state() == PlaybackWorkerState::kPreparedIdle);
  PlaybackLifecycleResult result{};
  BOOMPI_EXPECT(context, !fixture.lifecycle_results.TryPop(&result));
}

void FillLifecycleResultMailbox(boompi::test::TestContext& context,
                                PlaybackLifecycleResultMailbox* const queue) {
  for (std::uint32_t index = 0U;
       index < boompi::audio::kPlaybackLifecycleResultCapacity; ++index) {
    PlaybackLifecycleResult result{};
    result.type = PlaybackLifecycleResultType::kShutdown;
    result.shutdown.code = PlaybackShutdownResultCode::kActiveGeneration;
    result.shutdown.request_id = 1000U + index;
    BOOMPI_EXPECT(context, result.valid());
    BOOMPI_EXPECT(context, queue->TryPush(result));
  }
}

void FillCriticalMailbox(boompi::test::TestContext& context,
                         PlaybackCriticalStreamEventMailbox* const queue) {
  PlaybackCriticalStreamEvent event{};
  event.code = PlaybackCriticalStreamEventCode::kFaulted;
  event.event_sequence = 999U;
  event.generation = Generation(90U, 91U, 92U);
  event.pcm_incarnation = 7U;
  event.native_error = -1;
  BOOMPI_EXPECT(context, event.valid());
  BOOMPI_EXPECT(context, queue->TryPush(event));
}

void FillEosMailbox(boompi::test::TestContext& context,
                    PlaybackEndOfStreamEventMailbox* const queue) {
  for (std::uint32_t index = 0U;
       index < boompi::audio::kPlaybackEndOfStreamEventCapacity; ++index) {
    PlaybackEndOfStreamAcceptedEvent event{};
    event.event_sequence = 1000U + index;
    event.generation = Generation(80U, 81U, 82U);
    event.pcm_incarnation = 9U;
    event.accepted_generation_sample_frames = 1U;
    event.last_accepted_chunk_sequence = 1U;
    BOOMPI_EXPECT(context, event.valid());
    BOOMPI_EXPECT(context, queue->TryPush(event));
  }
}

void TestCreateInitializeAndColdFence(boompi::test::TestContext& context) {
  PlaybackWorkerCore unconfigured;
  PlaybackWorkerConfig config{};
  PlaybackWorkerEndpoints missing{};
  BOOMPI_EXPECT(
      context,
      !PlaybackWorkerCore::Create(config, missing, &unconfigured).ok());
  BOOMPI_EXPECT(context,
                unconfigured.state() == PlaybackWorkerState::kUnconfigured);
  BOOMPI_EXPECT(context, unconfigured.Step().code ==
                             PlaybackWorkerStepCode::kNotInitialized);

  WorkerFixture fixture;
  BOOMPI_EXPECT(
      context,
      !PlaybackWorkerCore::Create(config, fixture.Endpoints(), nullptr).ok());
  auto invalid_config = config;
  invalid_config.first_stream_event_sequence = 0U;
  BOOMPI_EXPECT(context,
                !PlaybackWorkerCore::Create(invalid_config, fixture.Endpoints(),
                                            &fixture.worker)
                     .ok());
  BOOMPI_EXPECT(context, fixture.CreateAndInitialize(context));
  BOOMPI_EXPECT(context, fixture.sink.prepare_call_count() == 1U);

  const auto generation = Generation(1U, 2U, 3U);
  BOOMPI_EXPECT(context,
                fixture.lifecycle_commands.TryPush(ArmCommand(1U, generation)));
  PlaybackLifecycleResult result{};
  BOOMPI_EXPECT(context, StepUntilLifecycleResult(&fixture, &result));
  BOOMPI_EXPECT(context, result.type == PlaybackLifecycleResultType::kArm);
  BOOMPI_EXPECT(context,
                result.arm.code == PlaybackArmResultCode::kFenceMismatch);
  BOOMPI_EXPECT(context, result.arm.observed_fence_epoch == 0U);
  BOOMPI_EXPECT(context, result.valid());
  BOOMPI_EXPECT(context,
                fixture.worker.state() == PlaybackWorkerState::kPreparedIdle);
  BOOMPI_EXPECT(context, fixture.sink.write_call_count() == 0U);
  BOOMPI_EXPECT(context, fixture.sink.drop_call_count() == 0U);

  auto invalid_arm = ArmCommand(2U, generation);
  invalid_arm.retired_pcm_incarnation = 1U;
  BOOMPI_EXPECT(context, fixture.lifecycle_commands.TryPush(invalid_arm));
  PlaybackLifecycleResult invalid_result{};
  BOOMPI_EXPECT(context, StepUntilLifecycleResult(&fixture, &invalid_result));
  BOOMPI_EXPECT(context, invalid_result.arm.code ==
                             PlaybackArmResultCode::kInvalidCommand);
  BOOMPI_EXPECT(context, invalid_result.arm.observed_fence_epoch == 0U);
  BOOMPI_EXPECT(context, invalid_result.valid());

  WorkerFixture initialize_fault_fixture;
  BOOMPI_EXPECT(context, PlaybackWorkerCore::Create(
                             config, initialize_fault_fixture.Endpoints(),
                             &initialize_fault_fixture.worker)
                             .ok());
  BOOMPI_EXPECT(context, initialize_fault_fixture.sink.PushPrepareResult(
                             {PcmPlaybackControlCode::kFatal, -5}));
  BOOMPI_EXPECT(context, !initialize_fault_fixture.worker.Initialize().ok());
  BOOMPI_EXPECT(context, initialize_fault_fixture.worker.state() ==
                             PlaybackWorkerState::kFaulted);
  BOOMPI_EXPECT(context, initialize_fault_fixture.lifecycle_commands.TryPush(
                             ArmCommand(3U, generation)));
  PlaybackLifecycleResult fault_result{};
  BOOMPI_EXPECT(context, StepUntilLifecycleResult(&initialize_fault_fixture,
                                                  &fault_result));
  BOOMPI_EXPECT(context,
                fault_result.arm.code == PlaybackArmResultCode::kFaulted);
  BOOMPI_EXPECT(context, fault_result.arm.observed_fence_epoch == 0U);
  BOOMPI_EXPECT(context, fault_result.valid());

  WorkerFixture retry_fixture;
  auto invalid_committer_config = config;
  invalid_committer_config.committer.initial_pcm_incarnation = 0U;
  BOOMPI_EXPECT(context, !PlaybackWorkerCore::Create(invalid_committer_config,
                                                     retry_fixture.Endpoints(),
                                                     &retry_fixture.worker)
                              .ok());
  BOOMPI_EXPECT(context, retry_fixture.worker.state() ==
                             PlaybackWorkerState::kUnconfigured);
  BOOMPI_EXPECT(context,
                PlaybackWorkerCore::Create(config, retry_fixture.Endpoints(),
                                           &retry_fixture.worker)
                    .ok());
  BOOMPI_EXPECT(context, retry_fixture.sink.PushPrepareResult(
                             {PcmPlaybackControlCode::kSucceeded, 0}));
  BOOMPI_EXPECT(context, retry_fixture.worker.Initialize().ok());
  BOOMPI_EXPECT(context, retry_fixture.worker.state() ==
                             PlaybackWorkerState::kPreparedIdle);
}

void TestMalformedEnvelopeAndCorrelatablePayloadPolicy(
    boompi::test::TestContext& context) {
  const auto generation = Generation(1U, 4U, 5U);

  auto arm_without_request = ArmCommand(0U, generation);
  ExpectUncorrelatableLifecycleInputRejected(context, arm_without_request);

  PlaybackLifecycleCommand duck_without_generation{};
  duck_without_generation.type = PlaybackLifecycleCommandType::kSetDucked;
  duck_without_generation.request_id = 4U;
  ExpectUncorrelatableLifecycleInputRejected(context, duck_without_generation);

  auto quiesce_without_request = QuiesceCommand(0U, generation, 1U);
  ExpectUncorrelatableLifecycleInputRejected(context, quiesce_without_request);

  auto confirm_without_generation = ConfirmReferenceCommand(5U, {}, 1U);
  ExpectUncorrelatableLifecycleInputRejected(context,
                                             confirm_without_generation);

  ExpectUncorrelatableLifecycleInputRejected(context, ShutdownCommand(0U));

  PlaybackLifecycleCommand unset{};
  unset.request_id = 6U;
  ExpectUncorrelatableLifecycleInputRejected(context, unset);

  PlaybackLifecycleCommand unknown{};
  unknown.type = static_cast<PlaybackLifecycleCommandType>(255U);
  unknown.request_id = 7U;
  unknown.generation = generation;
  ExpectUncorrelatableLifecycleInputRejected(context, unknown);

  WorkerFixture urgent_fixture;
  if (!urgent_fixture.CreateAndInitialize(context)) {
    return;
  }
  BOOMPI_EXPECT(context, urgent_fixture.urgent_cancel.TryPush(
                             CancelCommand(0U, generation, 2U)));
  BOOMPI_EXPECT(context, urgent_fixture.worker.Step().code ==
                             PlaybackWorkerStepCode::kInvalidControlInput);
  PlaybackLocalCancelCompletion local{};
  BOOMPI_EXPECT(context, !urgent_fixture.local_cancel_results.TryPop(&local));
  BOOMPI_EXPECT(context, urgent_fixture.worker.state() ==
                             PlaybackWorkerState::kPreparedIdle);

  WorkerFixture payload_fixture;
  if (!payload_fixture.CreateAndInitialize(context)) {
    return;
  }

  PlaybackLifecycleCommand invalid_duck{};
  invalid_duck.type = PlaybackLifecycleCommandType::kSetDucked;
  invalid_duck.request_id = 8U;
  invalid_duck.generation = generation;
  invalid_duck.retired_pcm_incarnation = 1U;
  BOOMPI_EXPECT(context,
                payload_fixture.lifecycle_commands.TryPush(invalid_duck));
  PlaybackLifecycleResult result{};
  BOOMPI_EXPECT(context, StepUntilLifecycleResult(&payload_fixture, &result));
  BOOMPI_EXPECT(context,
                result.duck.code == PlaybackDuckResultCode::kInvalidCommand);
  BOOMPI_EXPECT(context, result.valid());

  auto producer_not_stopped = QuiesceCommand(9U, generation, 1U);
  producer_not_stopped.value = false;
  BOOMPI_EXPECT(context, payload_fixture.lifecycle_commands.TryPush(
                             producer_not_stopped));
  result = PlaybackLifecycleResult{};
  BOOMPI_EXPECT(context, StepUntilLifecycleResult(&payload_fixture, &result));
  BOOMPI_EXPECT(context, result.ingress_quiesced.code ==
                             PlaybackIngressQuiesceCode::kProducerNotStopped);
  BOOMPI_EXPECT(context, result.valid());

  BOOMPI_EXPECT(context, payload_fixture.lifecycle_commands.TryPush(
                             ConfirmReferenceCommand(10U, generation, 0U)));
  result = PlaybackLifecycleResult{};
  BOOMPI_EXPECT(context, StepUntilLifecycleResult(&payload_fixture, &result));
  BOOMPI_EXPECT(context, result.reference_reset.code ==
                             PlaybackReferenceResetCode::kInvalidConfirmation);
  BOOMPI_EXPECT(context, result.valid());

  auto invalid_shutdown = ShutdownCommand(11U);
  invalid_shutdown.generation = generation;
  BOOMPI_EXPECT(context,
                payload_fixture.lifecycle_commands.TryPush(invalid_shutdown));
  result = PlaybackLifecycleResult{};
  BOOMPI_EXPECT(context, StepUntilLifecycleResult(&payload_fixture, &result));
  BOOMPI_EXPECT(context, result.shutdown.code ==
                             PlaybackShutdownResultCode::kInvalidCommand);
  BOOMPI_EXPECT(context, result.valid());

  BOOMPI_EXPECT(context, payload_fixture.urgent_cancel.TryPush(
                             CancelCommand(12U, generation, generation.epoch)));
  local = PlaybackLocalCancelCompletion{};
  BOOMPI_EXPECT(context, StepUntilLocalCancel(&payload_fixture, &local));
  BOOMPI_EXPECT(context,
                local.code == PlaybackLocalCancelCode::kInvalidCommand);
  BOOMPI_EXPECT(context, local.valid());
  BOOMPI_EXPECT(context, payload_fixture.worker.state() ==
                             PlaybackWorkerState::kPreparedIdle);
}

void TestTwoPhaseArmAndRendererOnlyUrgentCancel(
    boompi::test::TestContext& context) {
  WorkerFixture fixture;
  if (!fixture.CreateAndInitialize(context)) {
    return;
  }
  const auto generation = Generation(1U, 10U, 11U);
  BOOMPI_EXPECT(context, fixture.fence.AdvanceTo(generation.epoch).ok());
  BOOMPI_EXPECT(
      context, fixture.lifecycle_commands.TryPush(ArmCommand(20U, generation)));
  const auto arm_start = fixture.worker.Step();
  BOOMPI_EXPECT(context, arm_start.code == PlaybackWorkerStepCode::kProgress);
  BOOMPI_EXPECT(context,
                fixture.worker.state() == PlaybackWorkerState::kArming);

  FillLifecycleResultMailbox(context, &fixture.lifecycle_results);
  BOOMPI_EXPECT(context, fixture.fence.AdvanceTo(2U).ok());
  BOOMPI_EXPECT(context, fixture.urgent_cancel.TryPush(
                             CancelCommand(21U, generation, 2U)));

  PlaybackLocalCancelCompletion completion{};
  BOOMPI_EXPECT(context, StepUntilLocalCancel(&fixture, &completion));
  BOOMPI_EXPECT(context, completion.code ==
                             PlaybackLocalCancelCode::kRendererOnlyQuiesced);
  BOOMPI_EXPECT(context, completion.renderer_only_quiesced());
  BOOMPI_EXPECT(context, completion.renderer_disarmed_after_cancel_fence);
  BOOMPI_EXPECT(context, completion.committer_generation_never_armed);
  BOOMPI_EXPECT(context, completion.no_accepted_pcm_for_generation);
  BOOMPI_EXPECT(context, !completion.reference_reset_required);
  BOOMPI_EXPECT(context, fixture.sink.write_call_count() == 0U);
  BOOMPI_EXPECT(context, fixture.sink.drop_call_count() == 0U);
  BOOMPI_EXPECT(context, fixture.sink.prepare_call_count() == 1U);
  BOOMPI_EXPECT(context, fixture.lifecycle_results.size_approx() ==
                             boompi::audio::kPlaybackLifecycleResultCapacity);
  BOOMPI_EXPECT(context, fixture.worker.state() ==
                             PlaybackWorkerState::kAwaitingIngressQuiescence);
}

void TestUrgentCancelAfterCommitterArmBeforeArmAck(
    boompi::test::TestContext& context) {
  WorkerFixture fixture;
  if (!fixture.CreateAndInitialize(context)) {
    return;
  }
  const auto generation = Generation(1U, 14U, 15U);
  BOOMPI_EXPECT(context, fixture.fence.AdvanceTo(generation.epoch).ok());
  BOOMPI_EXPECT(
      context, fixture.lifecycle_commands.TryPush(ArmCommand(25U, generation)));
  BOOMPI_EXPECT(
      context, fixture.worker.Step().code == PlaybackWorkerStepCode::kProgress);
  BOOMPI_EXPECT(context,
                fixture.worker.state() == PlaybackWorkerState::kArming);
  BOOMPI_EXPECT(
      context, fixture.worker.Step().code == PlaybackWorkerStepCode::kProgress);
  BOOMPI_EXPECT(context,
                fixture.worker.state() == PlaybackWorkerState::kArming);
  PlaybackLifecycleResult unpublished_arm{};
  BOOMPI_EXPECT(context, !fixture.lifecycle_results.TryPop(&unpublished_arm));

  ScriptSuccessfulCancel(context, &fixture.sink);
  BOOMPI_EXPECT(context, fixture.fence.AdvanceTo(2U).ok());
  BOOMPI_EXPECT(context, fixture.urgent_cancel.TryPush(
                             CancelCommand(26U, generation, 2U)));
  PlaybackLocalCancelCompletion completion{};
  BOOMPI_EXPECT(context, StepUntilLocalCancel(&fixture, &completion));
  BOOMPI_EXPECT(context, completion.acknowledged());
  BOOMPI_EXPECT(context, completion.reference_reset_required);
  BOOMPI_EXPECT(context, fixture.sink.drop_call_count() == 1U);
  BOOMPI_EXPECT(context, fixture.sink.prepare_call_count() == 2U);

  BOOMPI_EXPECT(context, StepUntilLifecycleResult(&fixture, &unpublished_arm));
  BOOMPI_EXPECT(context,
                unpublished_arm.type == PlaybackLifecycleResultType::kArm);
  BOOMPI_EXPECT(context,
                unpublished_arm.arm.code ==
                    PlaybackArmResultCode::kCancelledBeforeAcknowledgement);
  BOOMPI_EXPECT(context, !unpublished_arm.arm.acknowledged());
  BOOMPI_EXPECT(context, unpublished_arm.arm.observed_fence_epoch == 2U);
  BOOMPI_EXPECT(context, fixture.worker.state() ==
                             PlaybackWorkerState::kAwaitingIngressQuiescence);
}

void TestArmRemainsArmingUntilAckPublish(boompi::test::TestContext& context) {
  WorkerFixture fixture;
  if (!fixture.CreateAndInitialize(context)) {
    return;
  }
  const auto generation = Generation(1U, 16U, 17U);
  BOOMPI_EXPECT(context, fixture.fence.AdvanceTo(generation.epoch).ok());
  BOOMPI_EXPECT(
      context, fixture.lifecycle_commands.TryPush(ArmCommand(27U, generation)));
  BOOMPI_EXPECT(
      context, fixture.worker.Step().code == PlaybackWorkerStepCode::kProgress);
  BOOMPI_EXPECT(context,
                fixture.worker.state() == PlaybackWorkerState::kArming);

  FillLifecycleResultMailbox(context, &fixture.lifecycle_results);
  BOOMPI_EXPECT(
      context, fixture.worker.Step().code == PlaybackWorkerStepCode::kProgress);
  BOOMPI_EXPECT(context,
                fixture.worker.state() == PlaybackWorkerState::kArming);
  BOOMPI_EXPECT(context, fixture.worker.Step().code ==
                             PlaybackWorkerStepCode::kOutputBackpressure);
  BOOMPI_EXPECT(context,
                fixture.worker.state() == PlaybackWorkerState::kArming);

  PlaybackLifecycleResult filler{};
  BOOMPI_EXPECT(context, fixture.lifecycle_results.TryPop(&filler));
  BOOMPI_EXPECT(
      context, fixture.worker.Step().code == PlaybackWorkerStepCode::kProgress);
  BOOMPI_EXPECT(context,
                fixture.worker.state() == PlaybackWorkerState::kActive);

  bool arm_ack_observed = false;
  PlaybackLifecycleResult queued{};
  while (fixture.lifecycle_results.TryPop(&queued)) {
    if (queued.type == PlaybackLifecycleResultType::kArm) {
      BOOMPI_EXPECT(context, queued.valid());
      BOOMPI_EXPECT(context, queued.arm.acknowledged());
      BOOMPI_EXPECT(context, queued.arm.request_id == 27U);
      arm_ack_observed = true;
    }
  }
  BOOMPI_EXPECT(context, arm_ack_observed);
  BOOMPI_EXPECT(context, fixture.sink.write_call_count() == 0U);
}

void TestNeverArmedCancelProof(boompi::test::TestContext& context) {
  WorkerFixture fixture;
  if (!fixture.CreateAndInitialize(context)) {
    return;
  }
  const auto generation = Generation(1U, 12U, 13U);
  BOOMPI_EXPECT(context, fixture.fence.AdvanceTo(2U).ok());
  BOOMPI_EXPECT(context, fixture.urgent_cancel.TryPush(
                             CancelCommand(30U, generation, 2U)));
  PlaybackLocalCancelCompletion completion{};
  BOOMPI_EXPECT(context, StepUntilLocalCancel(&fixture, &completion));
  BOOMPI_EXPECT(context,
                completion.code == PlaybackLocalCancelCode::kAlreadyQuiescent);
  BOOMPI_EXPECT(context, completion.already_quiescent());
  BOOMPI_EXPECT(context, completion.renderer_generation_never_armed);
  BOOMPI_EXPECT(context, completion.committer_generation_never_armed);
  BOOMPI_EXPECT(context, completion.committer_prepared_idle);
  BOOMPI_EXPECT(context, completion.no_accepted_pcm_for_generation);
  BOOMPI_EXPECT(context, fixture.sink.drop_call_count() == 0U);
}

void TestRenderSubmitAndFullCommit(boompi::test::TestContext& context) {
  WorkerFixture fixture;
  if (!fixture.CreateAndInitialize(context)) {
    return;
  }
  const auto generation = Generation(1U, 20U, 21U);
  if (!ArmWorker(context, &fixture, generation, 40U)) {
    return;
  }
  BOOMPI_EXPECT(context,
                fixture.sink.PushAccepted(
                    960U, 2000000U, 2001000U, 0U,
                    boompi::audio::PlaybackTimingSource::kSoftwareEstimate));
  BOOMPI_EXPECT(context, PublishFrame(&fixture.ingress,
                                      MakeTtsFrame(generation, 0U, 1000000U)));
  for (std::uint32_t step = 0U;
       step < 12U && fixture.sink.write_call_count() == 0U; ++step) {
    fixture.worker.Step();
  }
  BOOMPI_EXPECT(context, fixture.sink.write_call_count() == 1U);
  BOOMPI_EXPECT(context,
                fixture.worker.state() == PlaybackWorkerState::kActive);
  AcceptedRenderChunk48k accepted{};
  BOOMPI_EXPECT(context, PopAccepted(&fixture.accepted, &accepted));
  BOOMPI_EXPECT(context, accepted.HasValidLength());
  BOOMPI_EXPECT(context, accepted.accepted_samples == 960U);
  BOOMPI_EXPECT(context, accepted.absolute_source_offset_sample_frames == 0U);
  BOOMPI_EXPECT(context, accepted.completes_render_chunk);
  BOOMPI_EXPECT(context, !accepted.source_end_of_stream);
  BOOMPI_EXPECT(context, accepted.accepted_chunk_sequence == 1U);
  PlaybackEndOfStreamAcceptedEvent eos{};
  PlaybackCriticalStreamEvent critical{};
  BOOMPI_EXPECT(context, !fixture.eos_events.TryPop(&eos));
  BOOMPI_EXPECT(context, !fixture.critical_events.TryPop(&critical));
}

void TestWouldBlockAndPartialWrites(boompi::test::TestContext& context) {
  WorkerFixture fixture;
  if (!fixture.CreateAndInitialize(context)) {
    return;
  }
  const auto generation = Generation(1U, 30U, 31U);
  if (!ArmWorker(context, &fixture, generation, 50U)) {
    return;
  }
  BOOMPI_EXPECT(context, fixture.sink.PushWriteCode(
                             PcmPlaybackWriteCode::kWouldBlock, -11));
  BOOMPI_EXPECT(context,
                fixture.sink.PushAccepted(
                    240U, 3000000U, 3001000U, 16U,
                    boompi::audio::PlaybackTimingSource::kSoftwareEstimate));
  BOOMPI_EXPECT(context,
                fixture.sink.PushAccepted(
                    720U, 3020000U, 3021000U, 0U,
                    boompi::audio::PlaybackTimingSource::kSoftwareEstimate));
  BOOMPI_EXPECT(context, PublishFrame(&fixture.ingress,
                                      MakeTtsFrame(generation, 0U, 1100000U)));
  bool observed_sink_backpressure = false;
  for (std::uint32_t step = 0U;
       step < 20U && fixture.sink.write_call_count() < 3U; ++step) {
    const auto result = fixture.worker.Step();
    observed_sink_backpressure =
        observed_sink_backpressure ||
        result.code == PlaybackWorkerStepCode::kSinkBackpressure;
  }
  BOOMPI_EXPECT(context, observed_sink_backpressure);
  BOOMPI_EXPECT(context, fixture.sink.write_call_count() == 3U);
  AcceptedRenderChunk48k first{};
  AcceptedRenderChunk48k second{};
  BOOMPI_EXPECT(context, PopAccepted(&fixture.accepted, &first));
  BOOMPI_EXPECT(context, PopAccepted(&fixture.accepted, &second));
  BOOMPI_EXPECT(context, first.accepted_samples == 240U);
  BOOMPI_EXPECT(context, first.absolute_source_offset_sample_frames == 0U);
  BOOMPI_EXPECT(context, !first.completes_render_chunk);
  BOOMPI_EXPECT(context, second.accepted_samples == 720U);
  BOOMPI_EXPECT(context, second.absolute_source_offset_sample_frames == 240U);
  BOOMPI_EXPECT(context, second.completes_render_chunk);
  BOOMPI_EXPECT(context, first.accepted_chunk_sequence == 1U);
  BOOMPI_EXPECT(context, second.accepted_chunk_sequence == 2U);
}

void TestEosPrefixDrainAndEvent(boompi::test::TestContext& context) {
  WorkerFixture fixture;
  PlaybackWorkerConfig config{};
  config.first_stream_event_sequence = 41U;
  if (!fixture.CreateAndInitialize(context, config)) {
    return;
  }
  const auto generation = Generation(1U, 40U, 41U);
  if (!ArmWorker(context, &fixture, generation, 60U)) {
    return;
  }
  constexpr std::uint16_t kFinalSourceSamples = 73U;
  BOOMPI_EXPECT(context,
                fixture.sink.PushAccepted(
                    kFinalSourceSamples * 2U, 4000000U, 4001000U, 0U,
                    boompi::audio::PlaybackTimingSource::kSoftwareEstimate));
  BOOMPI_EXPECT(context,
                fixture.sink.PushAccepted(
                    63U, 4020000U, 4021000U, 0U,
                    boompi::audio::PlaybackTimingSource::kSoftwareEstimate));
  BOOMPI_EXPECT(
      context,
      PublishFrame(&fixture.ingress, MakeTtsFrame(generation, 0U, 1200000U, 500,
                                                  kFinalSourceSamples, true)));
  PlaybackEndOfStreamAcceptedEvent event{};
  bool event_observed = false;
  for (std::uint32_t step = 0U; step < 32U && !event_observed; ++step) {
    fixture.worker.Step();
    event_observed = fixture.eos_events.TryPop(&event);
  }
  BOOMPI_EXPECT(context, event_observed);
  BOOMPI_EXPECT(context, fixture.sink.write_call_count() == 2U);
  BOOMPI_EXPECT(context, event.valid());
  BOOMPI_EXPECT(context, event.event_sequence == 41U);
  BOOMPI_EXPECT(context, event.generation.epoch == generation.epoch);
  BOOMPI_EXPECT(context, event.generation.turn_id == generation.turn_id);
  BOOMPI_EXPECT(context, event.generation.stream_id == generation.stream_id);
  BOOMPI_EXPECT(context, event.pcm_incarnation == 1U);
  BOOMPI_EXPECT(context,
                event.accepted_generation_sample_frames ==
                    static_cast<std::uint64_t>(kFinalSourceSamples * 2U + 63U));
  BOOMPI_EXPECT(context, event.last_accepted_chunk_sequence == 2U);
  BOOMPI_EXPECT(context, fixture.worker.state() ==
                             PlaybackWorkerState::kEndOfStreamAccepted);

  AcceptedRenderChunk48k prefix{};
  AcceptedRenderChunk48k drain{};
  BOOMPI_EXPECT(context, PopAccepted(&fixture.accepted, &prefix));
  BOOMPI_EXPECT(context, PopAccepted(&fixture.accepted, &drain));
  BOOMPI_EXPECT(context, prefix.accepted_samples == kFinalSourceSamples * 2U);
  BOOMPI_EXPECT(context, !prefix.source_end_of_stream);
  BOOMPI_EXPECT(context, drain.accepted_samples == 63U);
  BOOMPI_EXPECT(context, drain.absolute_source_offset_sample_frames ==
                             kFinalSourceSamples * 2U);
  BOOMPI_EXPECT(context, drain.source_end_of_stream);
}

void TestEosEventBackpressureRetainsExactEvent(
    boompi::test::TestContext& context) {
  WorkerFixture fixture;
  PlaybackWorkerConfig config{};
  config.first_stream_event_sequence = 101U;
  if (!fixture.CreateAndInitialize(context, config)) {
    return;
  }
  const auto generation = Generation(1U, 42U, 43U);
  if (!ArmWorker(context, &fixture, generation, 65U)) {
    return;
  }
  FillEosMailbox(context, &fixture.eos_events);
  constexpr std::uint16_t kFinalSourceSamples = 5U;
  BOOMPI_EXPECT(context,
                fixture.sink.PushAccepted(
                    kFinalSourceSamples * 2U, 4100000U, 4101000U, 0U,
                    boompi::audio::PlaybackTimingSource::kSoftwareEstimate));
  BOOMPI_EXPECT(context,
                fixture.sink.PushAccepted(
                    63U, 4120000U, 4121000U, 0U,
                    boompi::audio::PlaybackTimingSource::kSoftwareEstimate));
  BOOMPI_EXPECT(
      context,
      PublishFrame(&fixture.ingress, MakeTtsFrame(generation, 0U, 1210000U, 700,
                                                  kFinalSourceSamples, true)));
  for (std::uint32_t step = 0U;
       step < 24U &&
       fixture.worker.state() != PlaybackWorkerState::kEndOfStreamAccepted;
       ++step) {
    fixture.worker.Step();
  }
  BOOMPI_EXPECT(context, fixture.worker.state() ==
                             PlaybackWorkerState::kEndOfStreamAccepted);
  BOOMPI_EXPECT(context, fixture.sink.write_call_count() == 2U);
  BOOMPI_EXPECT(context, fixture.worker.Step().code ==
                             PlaybackWorkerStepCode::kOutputBackpressure);

  PlaybackEndOfStreamAcceptedEvent filler{};
  BOOMPI_EXPECT(context, fixture.eos_events.TryPop(&filler));
  BOOMPI_EXPECT(context, filler.event_sequence == 1000U);
  BOOMPI_EXPECT(
      context, fixture.worker.Step().code == PlaybackWorkerStepCode::kProgress);

  for (std::uint32_t index = 1U;
       index < boompi::audio::kPlaybackEndOfStreamEventCapacity; ++index) {
    BOOMPI_EXPECT(context, fixture.eos_events.TryPop(&filler));
    BOOMPI_EXPECT(context, filler.event_sequence == 1000U + index);
  }
  PlaybackEndOfStreamAcceptedEvent retained{};
  BOOMPI_EXPECT(context, fixture.eos_events.TryPop(&retained));
  BOOMPI_EXPECT(context, retained.valid());
  BOOMPI_EXPECT(context, retained.event_sequence == 101U);
  BOOMPI_EXPECT(context, retained.generation.epoch == generation.epoch);
  BOOMPI_EXPECT(context,
                retained.accepted_generation_sample_frames ==
                    static_cast<std::uint64_t>(kFinalSourceSamples * 2U + 63U));
  BOOMPI_EXPECT(context, retained.last_accepted_chunk_sequence == 2U);
}

void TestCriticalRetentionAndUrgentActiveCancel(
    boompi::test::TestContext& context) {
  WorkerFixture fixture;
  PlaybackWorkerConfig config{};
  config.first_stream_event_sequence = 77U;
  if (!fixture.CreateAndInitialize(context, config)) {
    return;
  }
  const auto generation = Generation(1U, 50U, 51U);
  if (!ArmWorker(context, &fixture, generation, 70U)) {
    return;
  }
  FillCriticalMailbox(context, &fixture.critical_events);
  BOOMPI_EXPECT(context,
                fixture.sink.PushWriteCode(PcmPlaybackWriteCode::kXrun, -32));
  BOOMPI_EXPECT(context, PublishFrame(&fixture.ingress,
                                      MakeTtsFrame(generation, 0U, 1300000U)));
  for (std::uint32_t step = 0U;
       step < 16U &&
       fixture.worker.state() != PlaybackWorkerState::kCancellationRequired;
       ++step) {
    fixture.worker.Step();
  }
  BOOMPI_EXPECT(context, fixture.worker.state() ==
                             PlaybackWorkerState::kCancellationRequired);
  BOOMPI_EXPECT(context, fixture.sink.write_call_count() == 1U);
  BOOMPI_EXPECT(context, PublishFrame(&fixture.ingress,
                                      MakeTtsFrame(generation, 1U, 1320000U)));
  for (std::uint32_t step = 0U; step < 3U; ++step) {
    const auto result = fixture.worker.Step();
    BOOMPI_EXPECT(
        context,
        result.code == PlaybackWorkerStepCode::kOutputBackpressure ||
            result.code == PlaybackWorkerStepCode::kAwaitingCancellation);
  }
  BOOMPI_EXPECT(context, fixture.sink.write_call_count() == 1U);

  ScriptSuccessfulCancel(context, &fixture.sink);
  BOOMPI_EXPECT(context, fixture.fence.AdvanceTo(2U).ok());
  BOOMPI_EXPECT(context, fixture.urgent_cancel.TryPush(
                             CancelCommand(71U, generation, 2U)));
  PlaybackLocalCancelCompletion completion{};
  BOOMPI_EXPECT(context, StepUntilLocalCancel(&fixture, &completion));
  BOOMPI_EXPECT(context, completion.acknowledged());
  BOOMPI_EXPECT(context, completion.reference_reset_required);
  BOOMPI_EXPECT(context, fixture.sink.drop_call_count() == 1U);
  BOOMPI_EXPECT(context, fixture.sink.prepare_call_count() == 2U);

  PlaybackCriticalStreamEvent filler{};
  BOOMPI_EXPECT(context, fixture.critical_events.TryPop(&filler));
  BOOMPI_EXPECT(context, filler.event_sequence == 999U);
  PlaybackCriticalStreamEvent retained{};
  bool retained_observed = false;
  for (std::uint32_t step = 0U; step < 6U && !retained_observed; ++step) {
    fixture.worker.Step();
    retained_observed = fixture.critical_events.TryPop(&retained);
  }
  BOOMPI_EXPECT(context, retained_observed);
  BOOMPI_EXPECT(context, retained.valid());
  BOOMPI_EXPECT(context, retained.event_sequence == 77U);
  BOOMPI_EXPECT(
      context,
      retained.code == PlaybackCriticalStreamEventCode::kCancellationRequired);
  BOOMPI_EXPECT(context, retained.generation.epoch == generation.epoch);
  BOOMPI_EXPECT(context, retained.native_error == -32);
}

void TestFullIngressQuiesceReferenceConfirmAndRearm(
    boompi::test::TestContext& context) {
  WorkerFixture fixture;
  if (!fixture.CreateAndInitialize(context)) {
    return;
  }
  const auto generation = Generation(1U, 60U, 61U);
  if (!ArmWorker(context, &fixture, generation, 80U)) {
    return;
  }
  ScriptSuccessfulCancel(context, &fixture.sink);
  BOOMPI_EXPECT(context, fixture.fence.AdvanceTo(2U).ok());
  BOOMPI_EXPECT(context, fixture.urgent_cancel.TryPush(
                             CancelCommand(81U, generation, 2U)));
  PlaybackLocalCancelCompletion cancellation{};
  if (!StepUntilLocalCancel(&fixture, &cancellation)) {
    BOOMPI_EXPECT(context, false);
    return;
  }
  BOOMPI_EXPECT(context, cancellation.acknowledged());
  BOOMPI_EXPECT(context,
                cancellation.committer_result.retired_pcm_incarnation == 1U);

  BOOMPI_EXPECT(context,
                fixture.lifecycle_commands.TryPush(ConfirmReferenceCommand(
                    81U, generation,
                    cancellation.committer_result.retired_pcm_incarnation)));
  PlaybackLifecycleResult premature_confirmation{};
  BOOMPI_EXPECT(context,
                StepUntilLifecycleResult(&fixture, &premature_confirmation));
  BOOMPI_EXPECT(context, premature_confirmation.type ==
                             PlaybackLifecycleResultType::kReferenceReset);
  BOOMPI_EXPECT(context, premature_confirmation.reference_reset.code ==
                             PlaybackReferenceResetCode::kNotAwaiting);
  BOOMPI_EXPECT(context, !premature_confirmation.reference_reset.confirmed);
  BOOMPI_EXPECT(context, fixture.worker.state() ==
                             PlaybackWorkerState::kAwaitingIngressQuiescence);

  for (std::uint32_t index = 0U;
       index < boompi::audio::kTtsIngressQueueCapacity; ++index) {
    BOOMPI_EXPECT(
        context,
        PublishFrame(&fixture.ingress,
                     MakeTtsFrame(generation, index,
                                  2000000U + static_cast<std::uint64_t>(index) *
                                                 20000U)));
  }
  BOOMPI_EXPECT(context,
                fixture.lifecycle_commands.TryPush(QuiesceCommand(
                    81U, generation,
                    cancellation.committer_result.retired_pcm_incarnation)));
  PlaybackLifecycleResult progress{};
  BOOMPI_EXPECT(context, StepUntilLifecycleResult(&fixture, &progress));
  BOOMPI_EXPECT(context,
                progress.type == PlaybackLifecycleResultType::kIngressQuiesced);
  BOOMPI_EXPECT(context, progress.ingress_quiesced.code ==
                             PlaybackIngressQuiesceCode::kInProgress);
  BOOMPI_EXPECT(context, progress.ingress_quiesced.discarded_frames ==
                             boompi::audio::kTtsIngressQueueCapacity);
  BOOMPI_EXPECT(context, progress.ingress_quiesced.reached_iteration_bound);
  BOOMPI_EXPECT(context,
                !progress.ingress_quiesced.empty_observed_after_producer_stop);

  PlaybackLifecycleResult acknowledged{};
  BOOMPI_EXPECT(context, StepUntilLifecycleResult(&fixture, &acknowledged));
  BOOMPI_EXPECT(context, acknowledged.type ==
                             PlaybackLifecycleResultType::kIngressQuiesced);
  BOOMPI_EXPECT(context, acknowledged.ingress_quiesced.acknowledged());
  BOOMPI_EXPECT(context, acknowledged.ingress_quiesced.discarded_frames ==
                             boompi::audio::kTtsIngressQueueCapacity);
  BOOMPI_EXPECT(
      context,
      acknowledged.ingress_quiesced.empty_observed_after_producer_stop);
  BOOMPI_EXPECT(context, fixture.ingress.size_approx() == 0U);
  BOOMPI_EXPECT(context, fixture.worker.state() ==
                             PlaybackWorkerState::kAwaitingReferenceReset);

  BOOMPI_EXPECT(context,
                fixture.lifecycle_commands.TryPush(QuiesceCommand(
                    81U, generation,
                    cancellation.committer_result.retired_pcm_incarnation)));
  PlaybackLifecycleResult duplicate_quiesce{};
  BOOMPI_EXPECT(context,
                StepUntilLifecycleResult(&fixture, &duplicate_quiesce));
  BOOMPI_EXPECT(context, duplicate_quiesce.valid());
  BOOMPI_EXPECT(context, duplicate_quiesce.ingress_quiesced.acknowledged());
  BOOMPI_EXPECT(context, duplicate_quiesce.ingress_quiesced.discarded_frames ==
                             acknowledged.ingress_quiesced.discarded_frames);
  BOOMPI_EXPECT(context,
                duplicate_quiesce.ingress_quiesced.discarded_source_samples ==
                    acknowledged.ingress_quiesced.discarded_source_samples);
  BOOMPI_EXPECT(context, fixture.worker.state() ==
                             PlaybackWorkerState::kAwaitingReferenceReset);

  BOOMPI_EXPECT(context,
                fixture.lifecycle_commands.TryPush(ConfirmReferenceCommand(
                    81U, generation,
                    cancellation.committer_result.retired_pcm_incarnation)));
  PlaybackLifecycleResult confirmed{};
  BOOMPI_EXPECT(context, StepUntilLifecycleResult(&fixture, &confirmed));
  BOOMPI_EXPECT(context,
                confirmed.type == PlaybackLifecycleResultType::kReferenceReset);
  BOOMPI_EXPECT(context, confirmed.reference_reset.code ==
                             PlaybackReferenceResetCode::kConfirmed);
  BOOMPI_EXPECT(context, confirmed.reference_reset.confirmed);
  BOOMPI_EXPECT(context,
                fixture.worker.state() == PlaybackWorkerState::kPreparedIdle);

  BOOMPI_EXPECT(context,
                fixture.lifecycle_commands.TryPush(ConfirmReferenceCommand(
                    81U, generation,
                    cancellation.committer_result.retired_pcm_incarnation)));
  PlaybackLifecycleResult duplicate_confirmation{};
  BOOMPI_EXPECT(context,
                StepUntilLifecycleResult(&fixture, &duplicate_confirmation));
  BOOMPI_EXPECT(context, duplicate_confirmation.reference_reset.code ==
                             PlaybackReferenceResetCode::kDuplicateConfirmed);
  BOOMPI_EXPECT(context, duplicate_confirmation.reference_reset.confirmed);
  BOOMPI_EXPECT(context,
                fixture.worker.state() == PlaybackWorkerState::kPreparedIdle);

  const auto next_generation = Generation(2U, 62U, 63U);
  BOOMPI_EXPECT(context,
                ArmWorker(context, &fixture, next_generation, 82U, false));

  BOOMPI_EXPECT(context,
                fixture.lifecycle_commands.TryPush(QuiesceCommand(
                    81U, generation,
                    cancellation.committer_result.retired_pcm_incarnation)));
  PlaybackLifecycleResult historical_quiesce{};
  BOOMPI_EXPECT(context,
                StepUntilLifecycleResult(&fixture, &historical_quiesce));
  BOOMPI_EXPECT(context, historical_quiesce.valid());
  BOOMPI_EXPECT(context, historical_quiesce.ingress_quiesced.acknowledged());
  BOOMPI_EXPECT(context, historical_quiesce.ingress_quiesced.discarded_frames ==
                             acknowledged.ingress_quiesced.discarded_frames);
  BOOMPI_EXPECT(context,
                fixture.worker.state() == PlaybackWorkerState::kActive);

  BOOMPI_EXPECT(
      context,
      fixture.lifecycle_commands.TryPush(QuiesceCommand(
          81U, generation,
          cancellation.committer_result.retired_pcm_incarnation + 1U)));
  PlaybackLifecycleResult changed_incarnation{};
  BOOMPI_EXPECT(context,
                StepUntilLifecycleResult(&fixture, &changed_incarnation));
  BOOMPI_EXPECT(context, changed_incarnation.valid());
  BOOMPI_EXPECT(context, changed_incarnation.ingress_quiesced.code ==
                             PlaybackIngressQuiesceCode::kGenerationMismatch);
  BOOMPI_EXPECT(context,
                fixture.worker.state() == PlaybackWorkerState::kActive);

  auto changed_value = QuiesceCommand(
      81U, generation, cancellation.committer_result.retired_pcm_incarnation);
  changed_value.value = false;
  BOOMPI_EXPECT(context, fixture.lifecycle_commands.TryPush(changed_value));
  PlaybackLifecycleResult producer_not_stopped{};
  BOOMPI_EXPECT(context,
                StepUntilLifecycleResult(&fixture, &producer_not_stopped));
  BOOMPI_EXPECT(context, producer_not_stopped.valid());
  BOOMPI_EXPECT(context, producer_not_stopped.ingress_quiesced.code ==
                             PlaybackIngressQuiesceCode::kProducerNotStopped);
  BOOMPI_EXPECT(context,
                fixture.worker.state() == PlaybackWorkerState::kActive);
}

void TestOldCancelConflictPreservesSuccessCacheAndNextGeneration(
    boompi::test::TestContext& context) {
  WorkerFixture fixture;
  if (!fixture.CreateAndInitialize(context)) {
    return;
  }
  const auto old_generation = Generation(1U, 64U, 65U);
  if (!ArmWorker(context, &fixture, old_generation, 83U)) {
    return;
  }
  ScriptSuccessfulCancel(context, &fixture.sink);
  BOOMPI_EXPECT(context, fixture.fence.AdvanceTo(2U).ok());
  const auto original_cancel = CancelCommand(84U, old_generation, 2U);
  BOOMPI_EXPECT(context, fixture.urgent_cancel.TryPush(original_cancel));
  PlaybackLocalCancelCompletion original_completion{};
  BOOMPI_EXPECT(context, StepUntilLocalCancel(&fixture, &original_completion));
  BOOMPI_EXPECT(context, original_completion.acknowledged());

  BOOMPI_EXPECT(
      context,
      fixture.lifecycle_commands.TryPush(QuiesceCommand(
          original_cancel.request_id, old_generation,
          original_completion.committer_result.retired_pcm_incarnation)));
  PlaybackLifecycleResult quiesced{};
  BOOMPI_EXPECT(context, StepUntilLifecycleResult(&fixture, &quiesced));
  BOOMPI_EXPECT(context, quiesced.ingress_quiesced.acknowledged());
  BOOMPI_EXPECT(
      context,
      fixture.lifecycle_commands.TryPush(ConfirmReferenceCommand(
          original_cancel.request_id, old_generation,
          original_completion.committer_result.retired_pcm_incarnation)));
  PlaybackLifecycleResult confirmed{};
  BOOMPI_EXPECT(context, StepUntilLifecycleResult(&fixture, &confirmed));
  BOOMPI_EXPECT(context, confirmed.reference_reset.confirmed);
  BOOMPI_EXPECT(context,
                fixture.worker.state() == PlaybackWorkerState::kPreparedIdle);

  const auto conflicting_cancel = CancelCommand(85U, old_generation, 2U);
  BOOMPI_EXPECT(context, fixture.urgent_cancel.TryPush(conflicting_cancel));
  PlaybackLocalCancelCompletion rejected{};
  BOOMPI_EXPECT(context, StepUntilLocalCancel(&fixture, &rejected));
  BOOMPI_EXPECT(context,
                rejected.code == PlaybackLocalCancelCode::kGenerationMismatch);
  BOOMPI_EXPECT(context, !rejected.acknowledged());
  BOOMPI_EXPECT(context, fixture.sink.drop_call_count() == 1U);
  BOOMPI_EXPECT(context, fixture.sink.prepare_call_count() == 2U);

  BOOMPI_EXPECT(context, fixture.urgent_cancel.TryPush(original_cancel));
  PlaybackLocalCancelCompletion replayed{};
  BOOMPI_EXPECT(context, StepUntilLocalCancel(&fixture, &replayed));
  BOOMPI_EXPECT(context, replayed.acknowledged());
  BOOMPI_EXPECT(context, replayed.code == original_completion.code);
  BOOMPI_EXPECT(context, replayed.request_id == original_completion.request_id);
  BOOMPI_EXPECT(context, replayed.observed_fence_epoch ==
                             original_completion.observed_fence_epoch);
  BOOMPI_EXPECT(
      context,
      replayed.committer_result.retired_pcm_incarnation ==
          original_completion.committer_result.retired_pcm_incarnation);
  BOOMPI_EXPECT(
      context,
      replayed.committer_result.prepared_pcm_incarnation ==
          original_completion.committer_result.prepared_pcm_incarnation);
  BOOMPI_EXPECT(context, fixture.sink.drop_call_count() == 1U);
  BOOMPI_EXPECT(context, fixture.sink.prepare_call_count() == 2U);
  BOOMPI_EXPECT(context,
                fixture.worker.state() == PlaybackWorkerState::kPreparedIdle);

  const auto next_generation = Generation(2U, 66U, 67U);
  BOOMPI_EXPECT(context,
                ArmWorker(context, &fixture, next_generation, 86U, false));
  BOOMPI_EXPECT(context,
                fixture.worker.state() == PlaybackWorkerState::kActive);

  BOOMPI_EXPECT(
      context,
      fixture.lifecycle_commands.TryPush(ConfirmReferenceCommand(
          original_cancel.request_id, old_generation,
          original_completion.committer_result.retired_pcm_incarnation)));
  PlaybackLifecycleResult duplicate_confirmation{};
  BOOMPI_EXPECT(context,
                StepUntilLifecycleResult(&fixture, &duplicate_confirmation));
  BOOMPI_EXPECT(context, duplicate_confirmation.type ==
                             PlaybackLifecycleResultType::kReferenceReset);
  BOOMPI_EXPECT(context, duplicate_confirmation.reference_reset.code ==
                             PlaybackReferenceResetCode::kDuplicateConfirmed);
  BOOMPI_EXPECT(context, duplicate_confirmation.reference_reset.confirmed);
  BOOMPI_EXPECT(context,
                fixture.worker.state() == PlaybackWorkerState::kActive);

  BOOMPI_EXPECT(context, fixture.urgent_cancel.TryPush(original_cancel));
  PlaybackLocalCancelCompletion replayed_while_new_active{};
  BOOMPI_EXPECT(context,
                StepUntilLocalCancel(&fixture, &replayed_while_new_active));
  BOOMPI_EXPECT(context, replayed_while_new_active.acknowledged());
  BOOMPI_EXPECT(context, fixture.sink.drop_call_count() == 1U);
  BOOMPI_EXPECT(context, fixture.sink.prepare_call_count() == 2U);
  BOOMPI_EXPECT(context,
                fixture.worker.state() == PlaybackWorkerState::kActive);

  BOOMPI_EXPECT(context,
                fixture.sink.PushAccepted(
                    960U, 5000000U, 5001000U, 0U,
                    boompi::audio::PlaybackTimingSource::kSoftwareEstimate));
  BOOMPI_EXPECT(context,
                PublishFrame(&fixture.ingress,
                             MakeTtsFrame(next_generation, 0U, 1400000U)));
  for (std::uint32_t step = 0U;
       step < 12U && fixture.sink.write_call_count() == 0U; ++step) {
    fixture.worker.Step();
  }
  BOOMPI_EXPECT(context, fixture.sink.write_call_count() == 1U);
  AcceptedRenderChunk48k accepted{};
  BOOMPI_EXPECT(context, PopAccepted(&fixture.accepted, &accepted));
  BOOMPI_EXPECT(context, accepted.metadata.epoch == next_generation.epoch);
  BOOMPI_EXPECT(context, accepted.metadata.turn_id == next_generation.turn_id);
  BOOMPI_EXPECT(context,
                accepted.metadata.stream_id == next_generation.stream_id);
}

void TestUrgentCancelRewritesPendingShutdownAck(
    boompi::test::TestContext& context) {
  WorkerFixture fixture;
  if (!fixture.CreateAndInitialize(context)) {
    return;
  }
  FillLifecycleResultMailbox(context, &fixture.lifecycle_results);
  BOOMPI_EXPECT(context,
                fixture.lifecycle_commands.TryPush(ShutdownCommand(87U)));
  BOOMPI_EXPECT(
      context, fixture.worker.Step().code == PlaybackWorkerStepCode::kProgress);
  BOOMPI_EXPECT(context,
                fixture.worker.state() == PlaybackWorkerState::kPreparedIdle);
  BOOMPI_EXPECT(context, fixture.worker.Step().code ==
                             PlaybackWorkerStepCode::kOutputBackpressure);

  const auto generation = Generation(1U, 68U, 69U);
  BOOMPI_EXPECT(context, fixture.fence.AdvanceTo(2U).ok());
  BOOMPI_EXPECT(context, fixture.urgent_cancel.TryPush(
                             CancelCommand(88U, generation, 2U)));
  PlaybackLocalCancelCompletion cancellation{};
  BOOMPI_EXPECT(context, StepUntilLocalCancel(&fixture, &cancellation));
  BOOMPI_EXPECT(context, cancellation.already_quiescent());
  BOOMPI_EXPECT(context, fixture.worker.state() ==
                             PlaybackWorkerState::kAwaitingIngressQuiescence);

  PlaybackLifecycleResult queued{};
  BOOMPI_EXPECT(context, fixture.lifecycle_results.TryPop(&queued));
  BOOMPI_EXPECT(context, queued.shutdown.request_id == 1000U);
  BOOMPI_EXPECT(
      context, fixture.worker.Step().code == PlaybackWorkerStepCode::kProgress);
  for (std::uint32_t index = 1U;
       index < boompi::audio::kPlaybackLifecycleResultCapacity; ++index) {
    BOOMPI_EXPECT(context, fixture.lifecycle_results.TryPop(&queued));
    BOOMPI_EXPECT(context, queued.shutdown.request_id == 1000U + index);
  }
  PlaybackLifecycleResult rewritten{};
  BOOMPI_EXPECT(context, fixture.lifecycle_results.TryPop(&rewritten));
  BOOMPI_EXPECT(context,
                rewritten.type == PlaybackLifecycleResultType::kShutdown);
  BOOMPI_EXPECT(context, rewritten.shutdown.request_id == 87U);
  BOOMPI_EXPECT(context, rewritten.shutdown.code ==
                             PlaybackShutdownResultCode::kActiveGeneration);
  BOOMPI_EXPECT(context, !rewritten.shutdown.acknowledged());
  BOOMPI_EXPECT(context, fixture.worker.state() ==
                             PlaybackWorkerState::kAwaitingIngressQuiescence);
  BOOMPI_EXPECT(context,
                fixture.worker.state() != PlaybackWorkerState::kStopped);
}

void TestRejectedFutureArmDoesNotRaiseGenerationFloor(
    boompi::test::TestContext& context) {
  WorkerFixture fixture;
  if (!fixture.CreateAndInitialize(context)) {
    return;
  }
  const auto active_generation = Generation(1U, 70U, 71U);
  if (!ArmWorker(context, &fixture, active_generation, 89U)) {
    return;
  }
  const auto rejected_future = Generation(3U, 76U, 77U);
  BOOMPI_EXPECT(context, fixture.lifecycle_commands.TryPush(
                             ArmCommand(90U, rejected_future)));
  PlaybackLifecycleResult rejected_arm{};
  BOOMPI_EXPECT(context, StepUntilLifecycleResult(&fixture, &rejected_arm));
  BOOMPI_EXPECT(context,
                rejected_arm.type == PlaybackLifecycleResultType::kArm);
  BOOMPI_EXPECT(context, rejected_arm.arm.code ==
                             PlaybackArmResultCode::kCommitterRejected);
  BOOMPI_EXPECT(context,
                fixture.worker.state() == PlaybackWorkerState::kActive);

  ScriptSuccessfulCancel(context, &fixture.sink);
  BOOMPI_EXPECT(context, fixture.fence.AdvanceTo(2U).ok());
  BOOMPI_EXPECT(context, fixture.urgent_cancel.TryPush(
                             CancelCommand(91U, active_generation, 2U)));
  PlaybackLocalCancelCompletion cancellation{};
  BOOMPI_EXPECT(context, StepUntilLocalCancel(&fixture, &cancellation));
  BOOMPI_EXPECT(context, cancellation.acknowledged());
  BOOMPI_EXPECT(context,
                fixture.lifecycle_commands.TryPush(QuiesceCommand(
                    91U, active_generation,
                    cancellation.committer_result.retired_pcm_incarnation)));
  PlaybackLifecycleResult quiesced{};
  BOOMPI_EXPECT(context, StepUntilLifecycleResult(&fixture, &quiesced));
  BOOMPI_EXPECT(context, quiesced.ingress_quiesced.acknowledged());
  BOOMPI_EXPECT(context,
                fixture.lifecycle_commands.TryPush(ConfirmReferenceCommand(
                    91U, active_generation,
                    cancellation.committer_result.retired_pcm_incarnation)));
  PlaybackLifecycleResult confirmed{};
  BOOMPI_EXPECT(context, StepUntilLifecycleResult(&fixture, &confirmed));
  BOOMPI_EXPECT(context, confirmed.reference_reset.confirmed);

  const auto never_armed_generation = Generation(2U, 72U, 73U);
  BOOMPI_EXPECT(context, fixture.fence.AdvanceTo(3U).ok());
  BOOMPI_EXPECT(context, fixture.urgent_cancel.TryPush(
                             CancelCommand(92U, never_armed_generation, 3U)));
  PlaybackLocalCancelCompletion no_sink{};
  BOOMPI_EXPECT(context, StepUntilLocalCancel(&fixture, &no_sink));
  BOOMPI_EXPECT(context, no_sink.already_quiescent());
  BOOMPI_EXPECT(context,
                no_sink.code == PlaybackLocalCancelCode::kAlreadyQuiescent);
}

void TestEventSequenceExhaustionIsStickyAfterTeardown(
    boompi::test::TestContext& context) {
  WorkerFixture fixture;
  PlaybackWorkerConfig config{};
  config.first_stream_event_sequence =
      std::numeric_limits<std::uint64_t>::max();
  if (!fixture.CreateAndInitialize(context, config)) {
    return;
  }
  const auto generation = Generation(1U, 72U, 73U);
  if (!ArmWorker(context, &fixture, generation, 93U)) {
    return;
  }
  constexpr std::uint16_t kFinalSourceSamples = 3U;
  BOOMPI_EXPECT(context,
                fixture.sink.PushAccepted(
                    kFinalSourceSamples * 2U, 6000000U, 6001000U, 0U,
                    boompi::audio::PlaybackTimingSource::kSoftwareEstimate));
  BOOMPI_EXPECT(context,
                fixture.sink.PushAccepted(
                    63U, 6020000U, 6021000U, 0U,
                    boompi::audio::PlaybackTimingSource::kSoftwareEstimate));
  BOOMPI_EXPECT(
      context,
      PublishFrame(&fixture.ingress, MakeTtsFrame(generation, 0U, 1500000U, 900,
                                                  kFinalSourceSamples, true)));
  PlaybackEndOfStreamAcceptedEvent eos{};
  bool eos_observed = false;
  for (std::uint32_t step = 0U; step < 32U && !eos_observed; ++step) {
    fixture.worker.Step();
    eos_observed = fixture.eos_events.TryPop(&eos);
  }
  BOOMPI_EXPECT(context, eos_observed);
  BOOMPI_EXPECT(
      context, eos.event_sequence == std::numeric_limits<std::uint64_t>::max());
  BOOMPI_EXPECT(context, fixture.worker.state() ==
                             PlaybackWorkerState::kEndOfStreamAccepted);

  ScriptSuccessfulCancel(context, &fixture.sink);
  BOOMPI_EXPECT(context, fixture.fence.AdvanceTo(2U).ok());
  BOOMPI_EXPECT(context, fixture.urgent_cancel.TryPush(
                             CancelCommand(94U, generation, 2U)));
  PlaybackLocalCancelCompletion cancellation{};
  BOOMPI_EXPECT(context, StepUntilLocalCancel(&fixture, &cancellation));
  BOOMPI_EXPECT(context, cancellation.acknowledged());
  BOOMPI_EXPECT(context,
                fixture.lifecycle_commands.TryPush(QuiesceCommand(
                    94U, generation,
                    cancellation.committer_result.retired_pcm_incarnation)));
  PlaybackLifecycleResult quiesced{};
  BOOMPI_EXPECT(context, StepUntilLifecycleResult(&fixture, &quiesced));
  BOOMPI_EXPECT(context, quiesced.ingress_quiesced.acknowledged());
  BOOMPI_EXPECT(context,
                fixture.lifecycle_commands.TryPush(ConfirmReferenceCommand(
                    94U, generation,
                    cancellation.committer_result.retired_pcm_incarnation)));
  PlaybackLifecycleResult confirmed{};
  BOOMPI_EXPECT(context, StepUntilLifecycleResult(&fixture, &confirmed));
  BOOMPI_EXPECT(context, confirmed.reference_reset.confirmed);
  BOOMPI_EXPECT(context,
                fixture.worker.state() == PlaybackWorkerState::kFaulted);
  BOOMPI_EXPECT(context,
                fixture.worker.Step().code == PlaybackWorkerStepCode::kFaulted);

  const auto next_generation = Generation(2U, 74U, 75U);
  BOOMPI_EXPECT(context, fixture.lifecycle_commands.TryPush(
                             ArmCommand(95U, next_generation)));
  PlaybackLifecycleResult arm_result{};
  BOOMPI_EXPECT(context, StepUntilLifecycleResult(&fixture, &arm_result));
  BOOMPI_EXPECT(context, arm_result.type == PlaybackLifecycleResultType::kArm);
  BOOMPI_EXPECT(context,
                arm_result.arm.code == PlaybackArmResultCode::kFaulted);
  BOOMPI_EXPECT(context,
                fixture.worker.state() == PlaybackWorkerState::kFaulted);
  BOOMPI_EXPECT(context, fixture.sink.write_call_count() == 2U);
  BOOMPI_EXPECT(context, fixture.sink.drop_call_count() == 1U);

  BOOMPI_EXPECT(context,
                fixture.lifecycle_commands.TryPush(ShutdownCommand(96U)));
  PlaybackLifecycleResult shutdown{};
  BOOMPI_EXPECT(context, StepUntilLifecycleResult(&fixture, &shutdown));
  BOOMPI_EXPECT(context,
                shutdown.type == PlaybackLifecycleResultType::kShutdown);
  BOOMPI_EXPECT(context,
                shutdown.shutdown.code == PlaybackShutdownResultCode::kFaulted);
  BOOMPI_EXPECT(context, !shutdown.shutdown.acknowledged());
  BOOMPI_EXPECT(context,
                fixture.worker.state() == PlaybackWorkerState::kFaulted);
}

void TestPcmIncarnationExhaustionPublishesValidTerminalCompletion(
    boompi::test::TestContext& context) {
  WorkerFixture fixture;
  PlaybackWorkerConfig config{};
  config.committer.initial_pcm_incarnation =
      std::numeric_limits<std::uint64_t>::max();
  if (!fixture.CreateAndInitialize(context, config)) {
    return;
  }

  const auto generation = Generation(1U, 76U, 77U);
  if (!ArmWorker(context, &fixture, generation, 97U)) {
    return;
  }
  BOOMPI_EXPECT(context, fixture.sink.PushDropResult(
                             {PcmPlaybackControlCode::kSucceeded, 0}));
  BOOMPI_EXPECT(context, fixture.fence.AdvanceTo(2U).ok());
  BOOMPI_EXPECT(context, fixture.urgent_cancel.TryPush(
                             CancelCommand(98U, generation, 2U)));

  PlaybackLocalCancelCompletion completion{};
  BOOMPI_EXPECT(context, StepUntilLocalCancel(&fixture, &completion));
  BOOMPI_EXPECT(context,
                completion.code == PlaybackLocalCancelCode::kRestartRequired);
  BOOMPI_EXPECT(context, completion.committer_result.code ==
                             PlaybackCancelCode::kIncarnationExhausted);
  BOOMPI_EXPECT(context, completion.valid());
  BOOMPI_EXPECT(context, completion.terminal_restart_required());
  BOOMPI_EXPECT(context, !completion.acknowledged());
  BOOMPI_EXPECT(context, completion.committer_result.drop_succeeded);
  BOOMPI_EXPECT(context, !completion.committer_result.prepare_succeeded);
  BOOMPI_EXPECT(context, completion.committer_result.retired_pcm_incarnation ==
                             std::numeric_limits<std::uint64_t>::max());
  BOOMPI_EXPECT(context,
                completion.committer_result.prepared_pcm_incarnation == 0U);
  BOOMPI_EXPECT(context, fixture.sink.drop_call_count() == 1U);
  BOOMPI_EXPECT(context, fixture.sink.prepare_call_count() == 1U);
  BOOMPI_EXPECT(context,
                fixture.worker.state() == PlaybackWorkerState::kFaulted);
  BOOMPI_EXPECT(context,
                fixture.worker.Step().code == PlaybackWorkerStepCode::kFaulted);
}

void TestNonTeardownCancelsPreservePendingShutdownAck(
    boompi::test::TestContext& context) {
  {
    WorkerFixture fixture;
    if (!fixture.CreateAndInitialize(context)) {
      return;
    }
    BOOMPI_EXPECT(context, fixture.fence.AdvanceTo(1U).ok());
    BOOMPI_EXPECT(context,
                  fixture.lifecycle_commands.TryPush(ShutdownCommand(97U)));
    BOOMPI_EXPECT(context, fixture.worker.Step().code ==
                               PlaybackWorkerStepCode::kProgress);

    const auto generation = Generation(1U, 78U, 79U);
    BOOMPI_EXPECT(context, fixture.urgent_cancel.TryPush(CancelCommand(
                               98U, generation, generation.epoch)));
    PlaybackLocalCancelCompletion invalid{};
    BOOMPI_EXPECT(context, StepUntilLocalCancel(&fixture, &invalid));
    BOOMPI_EXPECT(context,
                  invalid.code == PlaybackLocalCancelCode::kInvalidCommand);
    BOOMPI_EXPECT(context, fixture.worker.Step().code ==
                               PlaybackWorkerStepCode::kProgress);
    PlaybackLifecycleResult shutdown{};
    BOOMPI_EXPECT(context, fixture.lifecycle_results.TryPop(&shutdown));
    BOOMPI_EXPECT(context, shutdown.shutdown.code ==
                               PlaybackShutdownResultCode::kAcknowledged);
    BOOMPI_EXPECT(context, shutdown.shutdown.acknowledged());
    BOOMPI_EXPECT(context,
                  fixture.worker.state() == PlaybackWorkerState::kStopped);
  }

  {
    WorkerFixture fixture;
    if (!fixture.CreateAndInitialize(context)) {
      return;
    }
    const auto generation = Generation(1U, 80U, 81U);
    const auto historical_cancel = CancelCommand(99U, generation, 2U);
    BOOMPI_EXPECT(context, fixture.fence.AdvanceTo(2U).ok());
    BOOMPI_EXPECT(context, fixture.urgent_cancel.TryPush(historical_cancel));
    PlaybackLocalCancelCompletion no_sink{};
    BOOMPI_EXPECT(context, StepUntilLocalCancel(&fixture, &no_sink));
    BOOMPI_EXPECT(context, no_sink.already_quiescent());
    BOOMPI_EXPECT(context, fixture.lifecycle_commands.TryPush(
                               QuiesceCommand(99U, generation, 0U)));
    PlaybackLifecycleResult quiesced{};
    BOOMPI_EXPECT(context, StepUntilLifecycleResult(&fixture, &quiesced));
    BOOMPI_EXPECT(context, quiesced.ingress_quiesced.acknowledged());
    BOOMPI_EXPECT(context,
                  fixture.worker.state() == PlaybackWorkerState::kPreparedIdle);

    BOOMPI_EXPECT(context, fixture.lifecycle_commands.TryPush(
                               QuiesceCommand(99U, generation, 0U)));
    PlaybackLifecycleResult duplicate_quiesce{};
    BOOMPI_EXPECT(context,
                  StepUntilLifecycleResult(&fixture, &duplicate_quiesce));
    BOOMPI_EXPECT(context, duplicate_quiesce.ingress_quiesced.acknowledged());
    BOOMPI_EXPECT(context, duplicate_quiesce.valid());
    BOOMPI_EXPECT(context,
                  fixture.worker.state() == PlaybackWorkerState::kPreparedIdle);

    BOOMPI_EXPECT(context,
                  fixture.lifecycle_commands.TryPush(ShutdownCommand(100U)));
    BOOMPI_EXPECT(context, fixture.worker.Step().code ==
                               PlaybackWorkerStepCode::kProgress);
    BOOMPI_EXPECT(context, fixture.urgent_cancel.TryPush(historical_cancel));
    PlaybackLocalCancelCompletion replayed{};
    BOOMPI_EXPECT(context, StepUntilLocalCancel(&fixture, &replayed));
    BOOMPI_EXPECT(context, replayed.already_quiescent());
    BOOMPI_EXPECT(context, fixture.worker.Step().code ==
                               PlaybackWorkerStepCode::kProgress);
    PlaybackLifecycleResult shutdown{};
    BOOMPI_EXPECT(context, fixture.lifecycle_results.TryPop(&shutdown));
    BOOMPI_EXPECT(context, shutdown.shutdown.code ==
                               PlaybackShutdownResultCode::kAcknowledged);
    BOOMPI_EXPECT(context, shutdown.shutdown.acknowledged());
    BOOMPI_EXPECT(context,
                  fixture.worker.state() == PlaybackWorkerState::kStopped);
    BOOMPI_EXPECT(context, fixture.sink.drop_call_count() == 0U);
  }
}

void TestShutdownIdleAndRejectActive(boompi::test::TestContext& context) {
  {
    WorkerFixture fixture;
    if (!fixture.CreateAndInitialize(context)) {
      return;
    }
    BOOMPI_EXPECT(context,
                  fixture.lifecycle_commands.TryPush(ShutdownCommand(90U)));
    PlaybackLifecycleResult result{};
    BOOMPI_EXPECT(context, StepUntilLifecycleResult(&fixture, &result));
    BOOMPI_EXPECT(context,
                  result.type == PlaybackLifecycleResultType::kShutdown);
    BOOMPI_EXPECT(context, result.shutdown.code ==
                               PlaybackShutdownResultCode::kAcknowledged);
    BOOMPI_EXPECT(context, result.shutdown.acknowledged());
    BOOMPI_EXPECT(context,
                  fixture.worker.state() == PlaybackWorkerState::kStopped);
    BOOMPI_EXPECT(context, fixture.worker.Step().code ==
                               PlaybackWorkerStepCode::kStopped);
  }

  {
    WorkerFixture fixture;
    if (!fixture.CreateAndInitialize(context)) {
      return;
    }
    const auto generation = Generation(1U, 70U, 71U);
    if (!ArmWorker(context, &fixture, generation, 91U)) {
      return;
    }
    BOOMPI_EXPECT(context,
                  fixture.lifecycle_commands.TryPush(ShutdownCommand(92U)));
    PlaybackLifecycleResult result{};
    BOOMPI_EXPECT(context, StepUntilLifecycleResult(&fixture, &result));
    BOOMPI_EXPECT(context, result.shutdown.code ==
                               PlaybackShutdownResultCode::kActiveGeneration);
    BOOMPI_EXPECT(context, !result.shutdown.acknowledged());
    BOOMPI_EXPECT(context,
                  fixture.worker.state() == PlaybackWorkerState::kActive);
  }
}

}  // namespace

int main() {
  boompi::test::TestContext context;
  TestCreateInitializeAndColdFence(context);
  TestMalformedEnvelopeAndCorrelatablePayloadPolicy(context);
  TestTwoPhaseArmAndRendererOnlyUrgentCancel(context);
  TestUrgentCancelAfterCommitterArmBeforeArmAck(context);
  TestArmRemainsArmingUntilAckPublish(context);
  TestNeverArmedCancelProof(context);
  TestRenderSubmitAndFullCommit(context);
  TestWouldBlockAndPartialWrites(context);
  TestEosPrefixDrainAndEvent(context);
  TestEosEventBackpressureRetainsExactEvent(context);
  TestCriticalRetentionAndUrgentActiveCancel(context);
  TestFullIngressQuiesceReferenceConfirmAndRearm(context);
  TestOldCancelConflictPreservesSuccessCacheAndNextGeneration(context);
  TestUrgentCancelRewritesPendingShutdownAck(context);
  TestRejectedFutureArmDoesNotRaiseGenerationFloor(context);
  TestEventSequenceExhaustionIsStickyAfterTeardown(context);
  TestPcmIncarnationExhaustionPublishesValidTerminalCompletion(context);
  TestNonTeardownCancelsPreservePendingShutdownAck(context);
  TestShutdownIdleAndRejectActive(context);

  if (context.failures() != 0U) {
    std::cerr << context.failures()
              << " playback worker expectation(s) failed\n";
    return 1;
  }
  std::cout << "boomPI playback worker tests passed\n";
  return 0;
}
