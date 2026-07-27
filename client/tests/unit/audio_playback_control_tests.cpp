#include <atomic>
#include <cstdint>
#include <iostream>
#include <thread>
#include <type_traits>

#include "boompi/audio/playback_control.h"
#include "boompi/event/spsc_pod_queue.h"
#include "boompi/test/test_context.h"

#define BOOMPI_EXPECT(context, expression) \
  (context).Expect((expression), #expression, __FILE__, __LINE__)

namespace {

using boompi::audio::DspReferenceResetAck;
using boompi::audio::DspReferenceResetCode;
using boompi::audio::DspReferenceResetRequest;
using boompi::audio::PlaybackCancelCode;
using boompi::audio::PlaybackCancelCommand;
using boompi::audio::PlaybackCancellationJoin;
using boompi::audio::PlaybackCancellationJoinCode;
using boompi::audio::PlaybackCancellationJoinState;
using boompi::audio::PlaybackCriticalStreamEvent;
using boompi::audio::PlaybackCriticalStreamEventCode;
using boompi::audio::PlaybackEndOfStreamAcceptedEvent;
using boompi::audio::PlaybackGeneration;
using boompi::audio::PlaybackIngressQuiesceCode;
using boompi::audio::PlaybackIngressQuiescedAck;
using boompi::audio::PlaybackLifecycleCommand;
using boompi::audio::PlaybackLifecycleCommandType;
using boompi::audio::PlaybackLifecycleResult;
using boompi::audio::PlaybackLifecycleResultType;
using boompi::audio::PlaybackLocalCancelCode;
using boompi::audio::PlaybackLocalCancelCompletion;
using boompi::audio::PlaybackReferenceResetCode;
using boompi::audio::PlaybackReferenceResetResult;
using boompi::audio::TtsProducerStartAck;
using boompi::audio::TtsProducerStartCode;
using boompi::audio::TtsProducerStartPermit;
using boompi::audio::TtsProducerStopAck;
using boompi::audio::TtsProducerStopCode;
using boompi::audio::TtsProducerStopRequest;

static_assert(std::is_trivially_copyable<PlaybackLifecycleCommand>::value,
              "lifecycle command must be trivially copyable");
static_assert(std::is_standard_layout<PlaybackLifecycleCommand>::value,
              "lifecycle command must be standard-layout");
static_assert(std::is_trivially_copyable<PlaybackCancelCommand>::value,
              "cancel command must be trivially copyable");
static_assert(std::is_standard_layout<PlaybackCancelCommand>::value,
              "cancel command must be standard-layout");
static_assert(std::is_trivially_copyable<PlaybackLifecycleResult>::value,
              "lifecycle result must be trivially copyable");
static_assert(std::is_standard_layout<PlaybackLifecycleResult>::value,
              "lifecycle result must be standard-layout");
static_assert(std::is_trivially_copyable<PlaybackLocalCancelCompletion>::value,
              "local cancel completion must be trivially copyable");
static_assert(std::is_standard_layout<PlaybackLocalCancelCompletion>::value,
              "local cancel completion must be standard-layout");
static_assert(boompi::audio::PlaybackUrgentCancelMailbox::capacity() == 1U,
              "urgent cancel lane must remain a single slot");
static_assert(boompi::audio::PlaybackLocalCancelResultMailbox::capacity() == 1U,
              "critical cancel result lane must remain a single slot");
static_assert(boompi::audio::PlaybackLifecycleCommandMailbox::capacity() == 8U,
              "lifecycle command capacity changed");
static_assert(boompi::audio::PlaybackLifecycleResultMailbox::capacity() == 16U,
              "lifecycle result capacity changed");
static_assert(boompi::audio::TtsProducerStartPermitMailbox::capacity() == 1U,
              "producer start lane must remain a single slot");
static_assert(boompi::audio::TtsProducerStopRequestMailbox::capacity() == 1U,
              "producer stop lane must remain a single slot");
static_assert(boompi::audio::DspReferenceResetRequestMailbox::capacity() == 1U,
              "DSP reset request lane must remain a single slot");
static_assert(boompi::audio::PlaybackEndOfStreamEventMailbox::capacity() == 16U,
              "playback EOS event capacity changed");
static_assert(boompi::audio::PlaybackCriticalStreamEventMailbox::capacity() ==
                  1U,
              "critical playback event lane must remain a single slot");

constexpr PlaybackGeneration kGeneration{4U, 7U, 9U};
constexpr std::uint64_t kRequestId = 31U;
constexpr std::uint64_t kRetiredIncarnation = 101U;
constexpr std::uint64_t kPreparedIncarnation = 102U;

PlaybackCancelCommand MakeCancelCommand() {
  return {kRequestId, kGeneration, 5U};
}

TtsProducerStopAck MakeProducerStopAck() {
  return {TtsProducerStopCode::kAcknowledged,
          kRequestId,
          kGeneration,
          5U,
          true,
          true};
}

PlaybackLocalCancelCompletion MakeLocalCancelCompletion() {
  PlaybackLocalCancelCompletion completion{};
  completion.code = PlaybackLocalCancelCode::kAcknowledged;
  completion.request_id = kRequestId;
  completion.generation = kGeneration;
  completion.observed_fence_epoch = 5U;
  completion.committer_result.code = PlaybackCancelCode::kAcknowledged;
  completion.committer_result.request_id = kRequestId;
  completion.committer_result.generation = kGeneration;
  completion.committer_result.observed_cancel_epoch = 5U;
  completion.committer_result.retired_pcm_incarnation = kRetiredIncarnation;
  completion.committer_result.prepared_pcm_incarnation = kPreparedIncarnation;
  completion.committer_result.accepted_generation_sample_frames = 2880U;
  completion.committer_result.last_accepted_chunk_sequence = 12U;
  completion.committer_result.accepted_after_cancel_fence_sample_frames = 7U;
  completion.committer_result.discarded_pending_sample_frames = 53U;
  completion.committer_result.drop_succeeded = true;
  completion.committer_result.prepare_succeeded = true;
  completion.committer_result.reference_reset_required = true;
  completion.committer_result.acknowledged = true;
  completion.initial_ingress_drain = {2U, 960U, false};
  completion.worker_holds_no_ingress_lease = true;
  completion.producer_lease_may_publish_late = true;
  completion.reference_reset_required = true;
  completion.renderer_disarmed_after_cancel_fence = true;
  return completion;
}

PlaybackLocalCancelCompletion MakeAlreadyQuiescentCompletion() {
  PlaybackLocalCancelCompletion completion{};
  completion.code = PlaybackLocalCancelCode::kAlreadyQuiescent;
  completion.request_id = kRequestId;
  completion.generation = kGeneration;
  completion.observed_fence_epoch = 5U;
  completion.worker_holds_no_ingress_lease = true;
  completion.producer_lease_may_publish_late = true;
  completion.renderer_generation_never_armed = true;
  completion.committer_generation_never_armed = true;
  completion.committer_prepared_idle = true;
  completion.no_accepted_pcm_for_generation = true;
  return completion;
}

PlaybackLocalCancelCompletion MakeRendererOnlyQuiescedCompletion() {
  PlaybackLocalCancelCompletion completion{};
  completion.code = PlaybackLocalCancelCode::kRendererOnlyQuiesced;
  completion.request_id = kRequestId;
  completion.generation = kGeneration;
  completion.observed_fence_epoch = 5U;
  completion.worker_holds_no_ingress_lease = true;
  completion.producer_lease_may_publish_late = true;
  completion.renderer_disarmed_after_cancel_fence = true;
  completion.committer_generation_never_armed = true;
  completion.committer_prepared_idle = true;
  completion.no_accepted_pcm_for_generation = true;
  return completion;
}

PlaybackIngressQuiescedAck MakeIngressQuiescedAck(
    const std::uint64_t retired_incarnation = kRetiredIncarnation) {
  return {PlaybackIngressQuiesceCode::kAcknowledged,
          kRequestId,
          kGeneration,
          retired_incarnation,
          3U,
          1440U,
          true,
          true,
          false};
}

DspReferenceResetAck MakeDspResetAck() {
  return {DspReferenceResetCode::kAcknowledged,
          kRequestId,
          kGeneration,
          kRetiredIncarnation,
          true,
          true,
          true,
          true};
}

DspReferenceResetAck MakeDspFailure(
    const DspReferenceResetCode code,
    const std::uint64_t retired_incarnation = kRetiredIncarnation) {
  return {code,  kRequestId, kGeneration, retired_incarnation,
          false, false,      false,       false};
}

PlaybackReferenceResetResult MakeReferenceResetResult() {
  return {PlaybackReferenceResetCode::kConfirmed, kRequestId, kGeneration,
          true};
}

void TestDefaultContractsFailClosed(boompi::test::TestContext& context) {
  constexpr PlaybackLifecycleCommand lifecycle{};
  constexpr PlaybackCancelCommand cancel{};
  constexpr PlaybackLifecycleResult result{};
  constexpr PlaybackLocalCancelCompletion local_cancel{};
  constexpr TtsProducerStopAck producer_stop{};
  constexpr DspReferenceResetAck dsp_reset{};
  constexpr PlaybackIngressQuiescedAck ingress{};
  constexpr PlaybackEndOfStreamAcceptedEvent eos{};
  constexpr PlaybackCriticalStreamEvent critical{};

  BOOMPI_EXPECT(context,
                lifecycle.type == PlaybackLifecycleCommandType::kUnset);
  BOOMPI_EXPECT(context, !lifecycle.valid());
  BOOMPI_EXPECT(context, !cancel.valid());
  BOOMPI_EXPECT(context, result.type == PlaybackLifecycleResultType::kUnset);
  BOOMPI_EXPECT(context, !result.valid());
  BOOMPI_EXPECT(context, local_cancel.code == PlaybackLocalCancelCode::kUnset);
  BOOMPI_EXPECT(context, !local_cancel.valid());
  BOOMPI_EXPECT(context, producer_stop.code == TtsProducerStopCode::kUnset);
  BOOMPI_EXPECT(context, !producer_stop.valid());
  BOOMPI_EXPECT(context, dsp_reset.code == DspReferenceResetCode::kUnset);
  BOOMPI_EXPECT(context, !dsp_reset.valid());
  BOOMPI_EXPECT(context, ingress.code == PlaybackIngressQuiesceCode::kUnset);
  BOOMPI_EXPECT(context, !ingress.valid());
  BOOMPI_EXPECT(context, !eos.valid());
  BOOMPI_EXPECT(context, !critical.valid());

  PlaybackCancellationJoin join;
  BOOMPI_EXPECT(context, join.state() == PlaybackCancellationJoinState::kIdle);
  BOOMPI_EXPECT(context, !join.can_arm_next_generation());
  const auto invalid_start = join.Begin(cancel);
  BOOMPI_EXPECT(context, invalid_start.code ==
                             PlaybackCancellationJoinCode::kInvalidInput);
  BOOMPI_EXPECT(context,
                invalid_start.state == PlaybackCancellationJoinState::kIdle);
  BOOMPI_EXPECT(context, !invalid_start.can_arm_next_generation);
}

void TestCommandValidation(boompi::test::TestContext& context) {
  PlaybackLifecycleCommand command{};
  command.type = PlaybackLifecycleCommandType::kArm;
  command.request_id = 1U;
  command.generation = kGeneration;
  BOOMPI_EXPECT(context, command.valid());
  command.value = true;
  BOOMPI_EXPECT(context, !command.valid());

  command = {};
  command.type = PlaybackLifecycleCommandType::kSetDucked;
  command.request_id = 2U;
  command.generation = kGeneration;
  command.value = true;
  BOOMPI_EXPECT(context, command.valid());
  command.retired_pcm_incarnation = 1U;
  BOOMPI_EXPECT(context, !command.valid());

  command = {};
  command.type = PlaybackLifecycleCommandType::kQuiesceIngress;
  command.request_id = 3U;
  command.generation = kGeneration;
  command.retired_pcm_incarnation = kRetiredIncarnation;
  BOOMPI_EXPECT(context, !command.valid());
  command.value = true;
  BOOMPI_EXPECT(context, command.valid());

  command = {};
  command.type = PlaybackLifecycleCommandType::kConfirmReferenceReset;
  command.request_id = 4U;
  command.generation = kGeneration;
  BOOMPI_EXPECT(context, !command.valid());
  command.retired_pcm_incarnation = kRetiredIncarnation;
  BOOMPI_EXPECT(context, command.valid());

  command = {};
  command.type = PlaybackLifecycleCommandType::kShutdown;
  command.request_id = 5U;
  BOOMPI_EXPECT(context, command.valid());
  command.generation = kGeneration;
  BOOMPI_EXPECT(context, !command.valid());

  auto cancel = MakeCancelCommand();
  BOOMPI_EXPECT(context, cancel.valid());
  cancel.cancel_epoch = cancel.generation.epoch;
  BOOMPI_EXPECT(context, !cancel.valid());
  cancel.cancel_epoch = 0U;
  BOOMPI_EXPECT(context, !cancel.valid());
}

void TestPodQueueFifoFullWrapAndPriority(boompi::test::TestContext& context) {
  boompi::audio::PlaybackLifecycleCommandMailbox ordinary;
  boompi::audio::PlaybackUrgentCancelMailbox urgent;

  BOOMPI_EXPECT(context, !ordinary.TryPop(nullptr));
  PlaybackLifecycleCommand popped{};
  BOOMPI_EXPECT(context, !ordinary.TryPop(&popped));

  for (std::uint64_t index = 0U;
       index < boompi::audio::kPlaybackLifecycleCommandCapacity; ++index) {
    PlaybackLifecycleCommand command{};
    command.type = PlaybackLifecycleCommandType::kArm;
    command.request_id = index + 1U;
    command.generation = {static_cast<std::uint32_t>(index + 1U), 1U, 1U};
    BOOMPI_EXPECT(context, command.valid());
    BOOMPI_EXPECT(context, ordinary.TryPush(command));
  }
  BOOMPI_EXPECT(context, ordinary.size_approx() ==
                             boompi::audio::kPlaybackLifecycleCommandCapacity);

  PlaybackLifecycleCommand overflow{};
  overflow.type = PlaybackLifecycleCommandType::kShutdown;
  overflow.request_id = 99U;
  BOOMPI_EXPECT(context, !ordinary.TryPush(overflow));

  const auto cancel = MakeCancelCommand();
  BOOMPI_EXPECT(context, urgent.TryPush(cancel));
  BOOMPI_EXPECT(context, !urgent.TryPush(cancel));
  PlaybackCancelCommand observed_cancel{};
  BOOMPI_EXPECT(context, urgent.TryPop(&observed_cancel));
  BOOMPI_EXPECT(context, observed_cancel.request_id == kRequestId);
  BOOMPI_EXPECT(context, observed_cancel.cancel_epoch == 5U);

  for (std::uint64_t expected = 1U;
       expected <= boompi::audio::kPlaybackLifecycleCommandCapacity;
       ++expected) {
    BOOMPI_EXPECT(context, ordinary.TryPop(&popped));
    BOOMPI_EXPECT(context, popped.request_id == expected);
  }
  BOOMPI_EXPECT(context, !ordinary.TryPop(&popped));

  for (std::uint64_t cycle = 0U; cycle < 64U; ++cycle) {
    PlaybackLifecycleCommand command{};
    command.type = PlaybackLifecycleCommandType::kShutdown;
    command.request_id = cycle + 100U;
    BOOMPI_EXPECT(context, ordinary.TryPush(command));
    BOOMPI_EXPECT(context, ordinary.TryPop(&popped));
    BOOMPI_EXPECT(context, popped.request_id == cycle + 100U);
  }
}

void TestEndpointPriorityLanesRemainIndependent(
    boompi::test::TestContext& context) {
  boompi::audio::TtsProducerStartPermitMailbox start_lane;
  boompi::audio::TtsProducerStopRequestMailbox stop_lane;
  const TtsProducerStartPermit start{kRequestId, kGeneration};
  const TtsProducerStopRequest stop{kRequestId, kGeneration, 5U};
  BOOMPI_EXPECT(context, start_lane.TryPush(start));
  BOOMPI_EXPECT(context, stop_lane.TryPush(stop));

  // The network-worker contract consumes Stop before inspecting Start when
  // both are ready. Separate lanes preserve both messages without allowing a
  // pending Start to apply backpressure to Stop.
  TtsProducerStopRequest observed_stop{};
  BOOMPI_EXPECT(context, stop_lane.TryPop(&observed_stop));
  BOOMPI_EXPECT(context, observed_stop.cancel_epoch == 5U);
  TtsProducerStartPermit observed_start{};
  BOOMPI_EXPECT(context, start_lane.TryPop(&observed_start));
  BOOMPI_EXPECT(context, observed_start.request_id == kRequestId);

  boompi::audio::PlaybackEndOfStreamEventMailbox eos_lane;
  for (std::uint64_t sequence = 1U;
       sequence <= boompi::audio::kPlaybackEndOfStreamEventCapacity;
       ++sequence) {
    PlaybackEndOfStreamAcceptedEvent eos{};
    eos.event_sequence = sequence;
    eos.generation = kGeneration;
    eos.pcm_incarnation = kPreparedIncarnation;
    eos.accepted_generation_sample_frames = sequence * 480U;
    eos.last_accepted_chunk_sequence = sequence;
    BOOMPI_EXPECT(context, eos.valid());
    BOOMPI_EXPECT(context, eos_lane.TryPush(eos));
  }
  PlaybackEndOfStreamAcceptedEvent eos_overflow{};
  eos_overflow.event_sequence = 99U;
  eos_overflow.generation = kGeneration;
  eos_overflow.pcm_incarnation = kPreparedIncarnation;
  eos_overflow.accepted_generation_sample_frames = 9600U;
  eos_overflow.last_accepted_chunk_sequence = 99U;
  BOOMPI_EXPECT(context, !eos_lane.TryPush(eos_overflow));

  boompi::audio::PlaybackCriticalStreamEventMailbox critical_lane;
  PlaybackCriticalStreamEvent cancellation{};
  cancellation.code = PlaybackCriticalStreamEventCode::kCancellationRequired;
  cancellation.event_sequence = 101U;
  cancellation.generation = kGeneration;
  cancellation.pcm_incarnation = kPreparedIncarnation;
  cancellation.accepted_generation_sample_frames = 480U;
  cancellation.last_accepted_chunk_sequence = 1U;
  BOOMPI_EXPECT(context, cancellation.valid());
  BOOMPI_EXPECT(context, critical_lane.TryPush(cancellation));

  PlaybackCriticalStreamEvent fault{};
  fault.code = PlaybackCriticalStreamEventCode::kFaulted;
  fault.event_sequence = 102U;
  fault.native_error = -5;
  BOOMPI_EXPECT(context, fault.valid());
  BOOMPI_EXPECT(context, !critical_lane.TryPush(fault));

  PlaybackCriticalStreamEvent observed_critical{};
  BOOMPI_EXPECT(context, critical_lane.TryPop(&observed_critical));
  BOOMPI_EXPECT(context,
                observed_critical.code ==
                    PlaybackCriticalStreamEventCode::kCancellationRequired);
  BOOMPI_EXPECT(context, observed_critical.event_sequence == 101U);
  BOOMPI_EXPECT(context, eos_lane.size_approx() ==
                             boompi::audio::kPlaybackEndOfStreamEventCapacity);
}

struct ConcurrentMessage final {
  std::uint32_t sequence{0U};
  std::uint32_t complement{0U};
};

void TestPodQueueConcurrentFifo(boompi::test::TestContext& context) {
  constexpr std::uint32_t kMessageCount = 100000U;
  boompi::event::SpscPodQueue<ConcurrentMessage, 64U> queue;
  std::atomic<bool> failed{false};

  std::thread producer([&queue, &failed]() {
    for (std::uint32_t sequence = 0U; sequence < kMessageCount; ++sequence) {
      const ConcurrentMessage message{sequence, ~sequence};
      while (!queue.TryPush(message)) {
        if (failed.load(std::memory_order_relaxed)) {
          return;
        }
        std::this_thread::yield();
      }
    }
  });

  std::thread consumer([&queue, &failed]() {
    for (std::uint32_t expected = 0U; expected < kMessageCount; ++expected) {
      ConcurrentMessage message{};
      while (!queue.TryPop(&message)) {
        if (failed.load(std::memory_order_relaxed)) {
          return;
        }
        std::this_thread::yield();
      }
      if (message.sequence != expected || message.complement != ~expected) {
        failed.store(true, std::memory_order_relaxed);
        return;
      }
    }
  });

  producer.join();
  consumer.join();
  BOOMPI_EXPECT(context, !failed.load(std::memory_order_relaxed));
  BOOMPI_EXPECT(context, queue.size_approx() == 0U);
}

void TestAckValidation(boompi::test::TestContext& context) {
  auto producer = MakeProducerStopAck();
  BOOMPI_EXPECT(context, producer.valid());
  BOOMPI_EXPECT(context, producer.acknowledged());
  producer.producer_write_lease_released = false;
  BOOMPI_EXPECT(context, !producer.valid());
  BOOMPI_EXPECT(context, !producer.acknowledged());

  auto partial_stop = MakeProducerStopAck();
  partial_stop.code = TtsProducerStopCode::kFailed;
  partial_stop.producer_write_lease_released = false;
  BOOMPI_EXPECT(context, partial_stop.valid());
  BOOMPI_EXPECT(context, !partial_stop.acknowledged());

  auto local = MakeLocalCancelCompletion();
  BOOMPI_EXPECT(context, local.valid());
  BOOMPI_EXPECT(context, local.acknowledged());
  BOOMPI_EXPECT(context, !local.already_quiescent());
  BOOMPI_EXPECT(context, !local.renderer_only_quiesced());
  BOOMPI_EXPECT(context, !local.terminal_restart_required());
  local.committer_result.prepared_pcm_incarnation = kRetiredIncarnation;
  BOOMPI_EXPECT(context, !local.valid());
  local = MakeLocalCancelCompletion();
  local.committer_result.prepared_pcm_incarnation = kPreparedIncarnation + 1U;
  BOOMPI_EXPECT(context, !local.valid());
  local = MakeLocalCancelCompletion();
  local.committer_result.native_error = -5;
  BOOMPI_EXPECT(context, !local.valid());

  auto quiescent = MakeAlreadyQuiescentCompletion();
  BOOMPI_EXPECT(context, quiescent.valid());
  BOOMPI_EXPECT(context, quiescent.already_quiescent());
  BOOMPI_EXPECT(context, !quiescent.acknowledged());
  BOOMPI_EXPECT(context, !quiescent.renderer_only_quiesced());
  quiescent.committer_result.acknowledged = true;
  quiescent.committer_result.reference_reset_required = true;
  quiescent.committer_result.retired_pcm_incarnation = kRetiredIncarnation;
  BOOMPI_EXPECT(context, !quiescent.valid());

  auto renderer_only = MakeRendererOnlyQuiescedCompletion();
  BOOMPI_EXPECT(context, renderer_only.valid());
  BOOMPI_EXPECT(context, renderer_only.renderer_only_quiesced());
  BOOMPI_EXPECT(context, !renderer_only.acknowledged());
  BOOMPI_EXPECT(context, !renderer_only.already_quiescent());
  renderer_only.renderer_disarmed_after_cancel_fence = false;
  BOOMPI_EXPECT(context, !renderer_only.valid());
  renderer_only = MakeRendererOnlyQuiescedCompletion();
  renderer_only.committer_prepared_idle = false;
  BOOMPI_EXPECT(context, !renderer_only.valid());

  auto fence_not_advanced = MakeLocalCancelCompletion();
  fence_not_advanced.code = PlaybackLocalCancelCode::kFenceNotAdvanced;
  fence_not_advanced.observed_fence_epoch = kGeneration.epoch;
  fence_not_advanced.committer_result = {};
  fence_not_advanced.committer_result.code =
      PlaybackCancelCode::kFenceNotAdvanced;
  fence_not_advanced.committer_result.request_id = kRequestId;
  fence_not_advanced.committer_result.generation = kGeneration;
  fence_not_advanced.committer_result.observed_cancel_epoch = kGeneration.epoch;
  fence_not_advanced.reference_reset_required = false;
  BOOMPI_EXPECT(context, fence_not_advanced.valid());

  auto mismatched_observation = MakeLocalCancelCompletion();
  mismatched_observation.committer_result.observed_cancel_epoch = 6U;
  BOOMPI_EXPECT(context, !mismatched_observation.valid());

  auto ingress = MakeIngressQuiescedAck();
  BOOMPI_EXPECT(context, ingress.valid());
  BOOMPI_EXPECT(context, ingress.acknowledged());
  ingress.reached_iteration_bound = true;
  BOOMPI_EXPECT(context, !ingress.valid());
  BOOMPI_EXPECT(context, !ingress.acknowledged());

  ingress = MakeIngressQuiescedAck();
  ingress.code = PlaybackIngressQuiesceCode::kInProgress;
  ingress.empty_observed_after_producer_stop = false;
  ingress.reached_iteration_bound = true;
  BOOMPI_EXPECT(context, ingress.valid());
  BOOMPI_EXPECT(context, !ingress.acknowledged());

  auto dsp = MakeDspResetAck();
  BOOMPI_EXPECT(context, dsp.valid());
  BOOMPI_EXPECT(context, dsp.acknowledged());
  dsp.dsp_history_reset = false;
  BOOMPI_EXPECT(context, !dsp.valid());
  BOOMPI_EXPECT(context, !dsp.acknowledged());
  dsp = MakeDspResetAck();
  dsp.accepted_queue_empty_after_local_cancel = false;
  BOOMPI_EXPECT(context, !dsp.valid());
  BOOMPI_EXPECT(context, !dsp.acknowledged());

  auto partial_dsp = MakeDspResetAck();
  partial_dsp.code = DspReferenceResetCode::kBackendFailed;
  partial_dsp.dsp_history_reset = false;
  BOOMPI_EXPECT(context, partial_dsp.valid());
  BOOMPI_EXPECT(context, !partial_dsp.acknowledged());
}

void TestWorkerEndpointProtocols(boompi::test::TestContext& context) {
  constexpr TtsProducerStartPermit empty_start{};
  constexpr TtsProducerStopRequest empty_stop{};
  constexpr TtsProducerStartAck empty_start_ack{};
  constexpr DspReferenceResetRequest empty_dsp_request{};
  constexpr PlaybackEndOfStreamAcceptedEvent empty_eos_event{};
  constexpr PlaybackCriticalStreamEvent empty_critical_event{};
  BOOMPI_EXPECT(context, !empty_start.valid());
  BOOMPI_EXPECT(context, !empty_stop.valid());
  BOOMPI_EXPECT(context, !empty_start_ack.valid());
  BOOMPI_EXPECT(context, !empty_start_ack.acknowledged());
  BOOMPI_EXPECT(context, !empty_dsp_request.valid());
  BOOMPI_EXPECT(context, !empty_eos_event.valid());
  BOOMPI_EXPECT(context, !empty_critical_event.valid());

  TtsProducerStartPermit start{kRequestId, kGeneration};
  BOOMPI_EXPECT(context, start.valid());
  start.generation.turn_id = 0U;
  BOOMPI_EXPECT(context, !start.valid());

  TtsProducerStopRequest stop{kRequestId, kGeneration, 5U};
  BOOMPI_EXPECT(context, stop.valid());
  stop.cancel_epoch = kGeneration.epoch;
  BOOMPI_EXPECT(context, !stop.valid());

  TtsProducerStartAck start_ack{TtsProducerStartCode::kAcknowledged, kRequestId,
                                kGeneration, true, true};
  BOOMPI_EXPECT(context, start_ack.valid());
  BOOMPI_EXPECT(context, start_ack.acknowledged());
  start_ack.owns_no_preexisting_write_lease = false;
  BOOMPI_EXPECT(context, !start_ack.valid());

  TtsProducerStartAck failed_start{TtsProducerStartCode::kFailed, kRequestId,
                                   kGeneration, false, true};
  BOOMPI_EXPECT(context, failed_start.valid());
  BOOMPI_EXPECT(context, !failed_start.acknowledged());

  DspReferenceResetRequest dsp_request{};
  dsp_request.request_id = kRequestId;
  dsp_request.generation = kGeneration;
  dsp_request.retired_pcm_incarnation = kRetiredIncarnation;
  dsp_request.last_accepted_chunk_sequence = 12U;
  dsp_request.accepted_generation_sample_frames = 2880U;
  BOOMPI_EXPECT(context, dsp_request.valid());
  dsp_request.retired_pcm_incarnation = 0U;
  BOOMPI_EXPECT(context, !dsp_request.valid());

  PlaybackEndOfStreamAcceptedEvent eos{};
  eos.event_sequence = 1U;
  eos.generation = kGeneration;
  eos.pcm_incarnation = kPreparedIncarnation;
  eos.accepted_generation_sample_frames = 3840U;
  eos.last_accepted_chunk_sequence = 14U;
  BOOMPI_EXPECT(context, eos.valid());
  eos.last_accepted_chunk_sequence = 0U;
  BOOMPI_EXPECT(context, !eos.valid());

  PlaybackCriticalStreamEvent cancellation{};
  cancellation.code = PlaybackCriticalStreamEventCode::kCancellationRequired;
  cancellation.event_sequence = 2U;
  cancellation.generation = kGeneration;
  cancellation.pcm_incarnation = kPreparedIncarnation;
  cancellation.accepted_generation_sample_frames = 960U;
  cancellation.last_accepted_chunk_sequence = 4U;
  cancellation.native_error = -32;
  BOOMPI_EXPECT(context, cancellation.valid());
  cancellation.last_accepted_chunk_sequence = 0U;
  BOOMPI_EXPECT(context, !cancellation.valid());
  cancellation.accepted_generation_sample_frames = 0U;
  BOOMPI_EXPECT(context, cancellation.valid());

  PlaybackCriticalStreamEvent cold_fault{};
  cold_fault.code = PlaybackCriticalStreamEventCode::kFaulted;
  cold_fault.event_sequence = 3U;
  cold_fault.native_error = -19;
  BOOMPI_EXPECT(context, cold_fault.valid());
  cold_fault.pcm_incarnation = kPreparedIncarnation;
  BOOMPI_EXPECT(context, !cold_fault.valid());

  PlaybackCriticalStreamEvent active_fault{};
  active_fault.code = PlaybackCriticalStreamEventCode::kFaulted;
  active_fault.event_sequence = 4U;
  active_fault.generation = kGeneration;
  active_fault.pcm_incarnation = kPreparedIncarnation;
  active_fault.native_error = -5;
  BOOMPI_EXPECT(context, active_fault.valid());
  active_fault.accepted_generation_sample_frames = 100U;
  BOOMPI_EXPECT(context, !active_fault.valid());
}

void TestCancellationJoinHappyPath(boompi::test::TestContext& context) {
  PlaybackCancellationJoin join;
  auto result = join.Begin(MakeCancelCommand());
  BOOMPI_EXPECT(context, result.code == PlaybackCancellationJoinCode::kStarted);
  BOOMPI_EXPECT(context, result.state ==
                             PlaybackCancellationJoinState::kAwaitingBarriers);
  BOOMPI_EXPECT(context, result.cancel_epoch == 5U);
  BOOMPI_EXPECT(context, !result.can_arm_next_generation);

  result = join.ObserveProducerStop(MakeProducerStopAck());
  BOOMPI_EXPECT(context,
                result.code == PlaybackCancellationJoinCode::kProgress);
  BOOMPI_EXPECT(context, result.producer_stopped);
  BOOMPI_EXPECT(context, !result.playback_local_cancelled);
  BOOMPI_EXPECT(context, !result.can_arm_next_generation);

  result = join.ObserveLocalCancel(MakeLocalCancelCompletion());
  BOOMPI_EXPECT(context,
                result.code == PlaybackCancellationJoinCode::kProgress);
  BOOMPI_EXPECT(context, result.playback_local_cancelled);
  BOOMPI_EXPECT(context, result.retired_pcm_incarnation == kRetiredIncarnation);
  BOOMPI_EXPECT(context, !result.ingress_quiesced);

  auto duplicate_local = MakeLocalCancelCompletion();
  duplicate_local.committer_result.code =
      PlaybackCancelCode::kDuplicateAcknowledged;
  result = join.ObserveLocalCancel(duplicate_local);
  BOOMPI_EXPECT(context,
                result.code == PlaybackCancellationJoinCode::kDuplicate);

  result = join.ObserveIngressQuiesced(MakeIngressQuiescedAck());
  BOOMPI_EXPECT(context,
                result.code == PlaybackCancellationJoinCode::kProgress);
  BOOMPI_EXPECT(context, result.ingress_quiesced);
  BOOMPI_EXPECT(context, !result.ready_for_playback_confirmation);

  result = join.ObserveDspReferenceReset(MakeDspResetAck());
  BOOMPI_EXPECT(
      context, result.code ==
                   PlaybackCancellationJoinCode::kReadyForPlaybackConfirmation);
  BOOMPI_EXPECT(
      context,
      result.state ==
          PlaybackCancellationJoinState::kReadyForPlaybackConfirmation);
  BOOMPI_EXPECT(context, result.dsp_reference_reset);
  BOOMPI_EXPECT(context, result.ready_for_playback_confirmation);
  BOOMPI_EXPECT(context, !result.can_arm_next_generation);
  BOOMPI_EXPECT(context, !join.can_arm_next_generation());

  result = join.ObservePlaybackReferenceReset(MakeReferenceResetResult());
  BOOMPI_EXPECT(context,
                result.code == PlaybackCancellationJoinCode::kCompleted);
  BOOMPI_EXPECT(context,
                result.state == PlaybackCancellationJoinState::kCompleted);
  BOOMPI_EXPECT(context, result.playback_reference_reset_confirmed);
  BOOMPI_EXPECT(context, result.can_arm_next_generation);
  BOOMPI_EXPECT(context, join.can_arm_next_generation());

  result = join.ObservePlaybackReferenceReset(MakeReferenceResetResult());
  BOOMPI_EXPECT(context,
                result.code == PlaybackCancellationJoinCode::kDuplicate);
  BOOMPI_EXPECT(context, result.can_arm_next_generation);
}

void TestCancellationJoinOrderingAndIdentity(
    boompi::test::TestContext& context) {
  PlaybackCancellationJoin join;
  BOOMPI_EXPECT(context, join.Begin(MakeCancelCommand()).code ==
                             PlaybackCancellationJoinCode::kStarted);

  auto result = join.ObserveIngressQuiesced(MakeIngressQuiescedAck());
  BOOMPI_EXPECT(context,
                result.code == PlaybackCancellationJoinCode::kOutOfOrder);
  BOOMPI_EXPECT(context, !result.ingress_quiesced);

  result = join.ObserveDspReferenceReset(MakeDspResetAck());
  BOOMPI_EXPECT(context,
                result.code == PlaybackCancellationJoinCode::kOutOfOrder);
  BOOMPI_EXPECT(context, !result.dsp_reference_reset);

  auto wrong_request = MakeProducerStopAck();
  wrong_request.request_id += 1U;
  result = join.ObserveProducerStop(wrong_request);
  BOOMPI_EXPECT(context,
                result.code == PlaybackCancellationJoinCode::kRequestMismatch);
  BOOMPI_EXPECT(context, !result.producer_stopped);

  auto wrong_generation = MakeProducerStopAck();
  wrong_generation.generation.stream_id += 1U;
  result = join.ObserveProducerStop(wrong_generation);
  BOOMPI_EXPECT(context, result.code ==
                             PlaybackCancellationJoinCode::kGenerationMismatch);
  BOOMPI_EXPECT(context, !result.producer_stopped);

  auto wrong_stop_epoch = MakeProducerStopAck();
  wrong_stop_epoch.observed_cancel_epoch = 6U;
  result = join.ObserveProducerStop(wrong_stop_epoch);
  BOOMPI_EXPECT(context, result.code ==
                             PlaybackCancellationJoinCode::kFenceEpochMismatch);
  BOOMPI_EXPECT(context, !result.producer_stopped);

  BOOMPI_EXPECT(context, join.ObserveProducerStop(MakeProducerStopAck()).code ==
                             PlaybackCancellationJoinCode::kProgress);
  BOOMPI_EXPECT(context, join.ObserveProducerStop(MakeProducerStopAck()).code ==
                             PlaybackCancellationJoinCode::kDuplicate);
  auto wrong_local_epoch = MakeLocalCancelCompletion();
  wrong_local_epoch.observed_fence_epoch = 6U;
  wrong_local_epoch.committer_result.observed_cancel_epoch = 6U;
  result = join.ObserveLocalCancel(wrong_local_epoch);
  BOOMPI_EXPECT(context, result.code ==
                             PlaybackCancellationJoinCode::kFenceEpochMismatch);
  BOOMPI_EXPECT(context, !result.playback_local_cancelled);

  BOOMPI_EXPECT(context,
                join.ObserveLocalCancel(MakeLocalCancelCompletion()).code ==
                    PlaybackCancellationJoinCode::kProgress);

  auto wrong_incarnation = MakeIngressQuiescedAck(kRetiredIncarnation + 1U);
  result = join.ObserveIngressQuiesced(wrong_incarnation);
  BOOMPI_EXPECT(
      context,
      result.code == PlaybackCancellationJoinCode::kIncarnationMismatch);
  BOOMPI_EXPECT(context, !result.ingress_quiesced);

  auto in_progress = MakeIngressQuiescedAck();
  in_progress.code = PlaybackIngressQuiesceCode::kInProgress;
  in_progress.empty_observed_after_producer_stop = false;
  in_progress.reached_iteration_bound = true;
  result = join.ObserveIngressQuiesced(in_progress);
  BOOMPI_EXPECT(context,
                result.code == PlaybackCancellationJoinCode::kProgress);
  BOOMPI_EXPECT(context, !result.ingress_quiesced);

  BOOMPI_EXPECT(context,
                join.ObserveIngressQuiesced(MakeIngressQuiescedAck()).code ==
                    PlaybackCancellationJoinCode::kProgress);
  BOOMPI_EXPECT(
      context, join.ObserveDspReferenceReset(MakeDspResetAck()).code ==
                   PlaybackCancellationJoinCode::kReadyForPlaybackConfirmation);

  auto stale_confirmation = MakeReferenceResetResult();
  stale_confirmation.generation.turn_id += 1U;
  result = join.ObservePlaybackReferenceReset(stale_confirmation);
  BOOMPI_EXPECT(context, result.code ==
                             PlaybackCancellationJoinCode::kGenerationMismatch);
  BOOMPI_EXPECT(context, !result.can_arm_next_generation);
}

void TestAlreadyQuiescentPath(boompi::test::TestContext& context) {
  PlaybackCancellationJoin join;
  BOOMPI_EXPECT(context, join.Begin(MakeCancelCommand()).code ==
                             PlaybackCancellationJoinCode::kStarted);
  BOOMPI_EXPECT(
      context, join.ObserveLocalCancel(MakeAlreadyQuiescentCompletion()).code ==
                   PlaybackCancellationJoinCode::kProgress);
  BOOMPI_EXPECT(context, join.ObserveProducerStop(MakeProducerStopAck()).code ==
                             PlaybackCancellationJoinCode::kProgress);

  auto ingress = MakeIngressQuiescedAck(0U);
  const auto result = join.ObserveIngressQuiesced(ingress);
  BOOMPI_EXPECT(context,
                result.code == PlaybackCancellationJoinCode::kCompleted);
  BOOMPI_EXPECT(context, result.can_arm_next_generation);
  BOOMPI_EXPECT(context, !result.dsp_reference_reset);
  BOOMPI_EXPECT(context, !result.playback_reference_reset_confirmed);
}

void TestRendererOnlyArmCancelRace(boompi::test::TestContext& context) {
  PlaybackCancellationJoin join;
  BOOMPI_EXPECT(context, join.Begin(MakeCancelCommand()).code ==
                             PlaybackCancellationJoinCode::kStarted);
  BOOMPI_EXPECT(
      context,
      join.ObserveLocalCancel(MakeRendererOnlyQuiescedCompletion()).code ==
          PlaybackCancellationJoinCode::kProgress);
  BOOMPI_EXPECT(context, join.ObserveProducerStop(MakeProducerStopAck()).code ==
                             PlaybackCancellationJoinCode::kProgress);

  // A renderer-only race never produced a PCM incarnation, but still waits
  // for a producer-stop-stable empty observation before permitting new Arm.
  const auto result = join.ObserveIngressQuiesced(MakeIngressQuiescedAck(0U));
  BOOMPI_EXPECT(context,
                result.code == PlaybackCancellationJoinCode::kCompleted);
  BOOMPI_EXPECT(context, result.can_arm_next_generation);
  BOOMPI_EXPECT(context, result.retired_pcm_incarnation == 0U);
  BOOMPI_EXPECT(context, !result.dsp_reference_reset);
  BOOMPI_EXPECT(context, !result.playback_reference_reset_confirmed);
}

void TestCompletedJoinRejectsEpochBelowAdvancedFence(
    boompi::test::TestContext& context) {
  PlaybackCancellationJoin join;
  auto wide_cancel = MakeCancelCommand();
  wide_cancel.cancel_epoch = 100U;
  BOOMPI_EXPECT(context, join.Begin(wide_cancel).code ==
                             PlaybackCancellationJoinCode::kStarted);

  auto producer = MakeProducerStopAck();
  producer.observed_cancel_epoch = 100U;
  BOOMPI_EXPECT(context, join.ObserveProducerStop(producer).code ==
                             PlaybackCancellationJoinCode::kProgress);

  auto local = MakeAlreadyQuiescentCompletion();
  local.observed_fence_epoch = 100U;
  BOOMPI_EXPECT(context, join.ObserveLocalCancel(local).code ==
                             PlaybackCancellationJoinCode::kProgress);
  BOOMPI_EXPECT(context,
                join.ObserveIngressQuiesced(MakeIngressQuiescedAck(0U)).code ==
                    PlaybackCancellationJoinCode::kCompleted);

  auto stale_floor = MakeCancelCommand();
  stale_floor.request_id = kRequestId + 1U;
  stale_floor.generation = {5U, 8U, 10U};
  stale_floor.cancel_epoch = 6U;
  const auto rejected = join.Begin(stale_floor);
  BOOMPI_EXPECT(context, rejected.code ==
                             PlaybackCancellationJoinCode::kEpochNotIncreasing);
  BOOMPI_EXPECT(context, rejected.can_arm_next_generation);

  auto legal_floor = stale_floor;
  legal_floor.generation.epoch = 100U;
  legal_floor.cancel_epoch = 101U;
  const auto started = join.Begin(legal_floor);
  BOOMPI_EXPECT(context,
                started.code == PlaybackCancellationJoinCode::kStarted);
  BOOMPI_EXPECT(context, !started.can_arm_next_generation);
}

void TestCompletedTransactionCannotBeReplayed(
    boompi::test::TestContext& context) {
  PlaybackCancellationJoin join;
  BOOMPI_EXPECT(context, join.Begin(MakeCancelCommand()).code ==
                             PlaybackCancellationJoinCode::kStarted);
  BOOMPI_EXPECT(context, join.ObserveProducerStop(MakeProducerStopAck()).code ==
                             PlaybackCancellationJoinCode::kProgress);
  BOOMPI_EXPECT(context,
                join.ObserveLocalCancel(MakeLocalCancelCompletion()).code ==
                    PlaybackCancellationJoinCode::kProgress);
  BOOMPI_EXPECT(context,
                join.ObserveIngressQuiesced(MakeIngressQuiescedAck()).code ==
                    PlaybackCancellationJoinCode::kProgress);
  BOOMPI_EXPECT(
      context, join.ObserveDspReferenceReset(MakeDspResetAck()).code ==
                   PlaybackCancellationJoinCode::kReadyForPlaybackConfirmation);
  BOOMPI_EXPECT(
      context,
      join.ObservePlaybackReferenceReset(MakeReferenceResetResult()).code ==
          PlaybackCancellationJoinCode::kCompleted);

  auto result = join.Begin(MakeCancelCommand());
  BOOMPI_EXPECT(context,
                result.code == PlaybackCancellationJoinCode::kDuplicate);
  BOOMPI_EXPECT(context, result.can_arm_next_generation);

  auto conflicting = MakeCancelCommand();
  conflicting.cancel_epoch = 6U;
  result = join.Begin(conflicting);
  BOOMPI_EXPECT(
      context,
      result.code == PlaybackCancellationJoinCode::kConflictingDuplicate);
  BOOMPI_EXPECT(context, result.can_arm_next_generation);

  auto reused_request = MakeCancelCommand();
  reused_request.generation = {5U, 8U, 10U};
  reused_request.cancel_epoch = 6U;
  result = join.Begin(reused_request);
  BOOMPI_EXPECT(
      context,
      result.code == PlaybackCancellationJoinCode::kRequestNotIncreasing);
  BOOMPI_EXPECT(context, result.can_arm_next_generation);

  auto reused_epoch = reused_request;
  reused_epoch.request_id = kRequestId + 1U;
  reused_epoch.generation.epoch = kGeneration.epoch;
  reused_epoch.cancel_epoch = 6U;
  result = join.Begin(reused_epoch);
  BOOMPI_EXPECT(context, result.code ==
                             PlaybackCancellationJoinCode::kEpochNotIncreasing);
  BOOMPI_EXPECT(context, result.can_arm_next_generation);

  auto next = reused_request;
  next.request_id = kRequestId + 1U;
  result = join.Begin(next);
  BOOMPI_EXPECT(context, result.code == PlaybackCancellationJoinCode::kStarted);
  BOOMPI_EXPECT(context, !result.can_arm_next_generation);
}

void TestConflictingDuplicateCannotChangeCancellationMode(
    boompi::test::TestContext& context) {
  PlaybackCancellationJoin join;
  BOOMPI_EXPECT(context, join.Begin(MakeCancelCommand()).code ==
                             PlaybackCancellationJoinCode::kStarted);
  BOOMPI_EXPECT(
      context, join.ObserveLocalCancel(MakeAlreadyQuiescentCompletion()).code ==
                   PlaybackCancellationJoinCode::kProgress);

  const auto conflict = join.ObserveLocalCancel(MakeLocalCancelCompletion());
  BOOMPI_EXPECT(
      context,
      conflict.code == PlaybackCancellationJoinCode::kConflictingDuplicate);
  BOOMPI_EXPECT(context,
                conflict.state == PlaybackCancellationJoinState::kFaulted);
  BOOMPI_EXPECT(context, !conflict.can_arm_next_generation);
}

void TestWrongIncarnationFailureCannotPoisonTransaction(
    boompi::test::TestContext& context) {
  PlaybackCancellationJoin join;
  BOOMPI_EXPECT(context, join.Begin(MakeCancelCommand()).code ==
                             PlaybackCancellationJoinCode::kStarted);
  BOOMPI_EXPECT(context, join.ObserveProducerStop(MakeProducerStopAck()).code ==
                             PlaybackCancellationJoinCode::kProgress);
  BOOMPI_EXPECT(context,
                join.ObserveLocalCancel(MakeLocalCancelCompletion()).code ==
                    PlaybackCancellationJoinCode::kProgress);

  auto wrong_progress = MakeIngressQuiescedAck(kRetiredIncarnation + 1U);
  wrong_progress.code = PlaybackIngressQuiesceCode::kInProgress;
  wrong_progress.empty_observed_after_producer_stop = false;
  wrong_progress.reached_iteration_bound = true;
  auto result = join.ObserveIngressQuiesced(wrong_progress);
  BOOMPI_EXPECT(
      context,
      result.code == PlaybackCancellationJoinCode::kIncarnationMismatch);
  BOOMPI_EXPECT(context, result.state ==
                             PlaybackCancellationJoinState::kAwaitingBarriers);

  BOOMPI_EXPECT(context,
                join.ObserveIngressQuiesced(MakeIngressQuiescedAck()).code ==
                    PlaybackCancellationJoinCode::kProgress);

  auto wrong_failure = MakeDspFailure(DspReferenceResetCode::kBackendFailed,
                                      kRetiredIncarnation + 1U);
  BOOMPI_EXPECT(context, wrong_failure.valid());
  result = join.ObserveDspReferenceReset(wrong_failure);
  BOOMPI_EXPECT(
      context,
      result.code == PlaybackCancellationJoinCode::kIncarnationMismatch);
  BOOMPI_EXPECT(context, result.state ==
                             PlaybackCancellationJoinState::kAwaitingBarriers);
  BOOMPI_EXPECT(context, !result.can_arm_next_generation);
}

void TestContradictoryAckAfterReadyRevokesPermit(
    boompi::test::TestContext& context) {
  PlaybackCancellationJoin join;
  BOOMPI_EXPECT(context, join.Begin(MakeCancelCommand()).code ==
                             PlaybackCancellationJoinCode::kStarted);
  BOOMPI_EXPECT(context, join.ObserveProducerStop(MakeProducerStopAck()).code ==
                             PlaybackCancellationJoinCode::kProgress);
  BOOMPI_EXPECT(context,
                join.ObserveLocalCancel(MakeLocalCancelCompletion()).code ==
                    PlaybackCancellationJoinCode::kProgress);
  BOOMPI_EXPECT(context,
                join.ObserveIngressQuiesced(MakeIngressQuiescedAck()).code ==
                    PlaybackCancellationJoinCode::kProgress);
  BOOMPI_EXPECT(
      context, join.ObserveDspReferenceReset(MakeDspResetAck()).code ==
                   PlaybackCancellationJoinCode::kReadyForPlaybackConfirmation);

  BOOMPI_EXPECT(context,
                join.ObserveDspReferenceReset(MakeDspResetAck()).code ==
                    PlaybackCancellationJoinCode::kDuplicate);
  auto contradiction = MakeDspFailure(DspReferenceResetCode::kBackendFailed);
  BOOMPI_EXPECT(context, contradiction.valid());
  const auto result = join.ObserveDspReferenceReset(contradiction);
  BOOMPI_EXPECT(
      context,
      result.code == PlaybackCancellationJoinCode::kConflictingDuplicate);
  BOOMPI_EXPECT(context,
                result.state == PlaybackCancellationJoinState::kFaulted);
  BOOMPI_EXPECT(context, !result.can_arm_next_generation);
}

void TestIngressCannotRegressAfterEmptyAck(boompi::test::TestContext& context) {
  PlaybackCancellationJoin join;
  BOOMPI_EXPECT(context, join.Begin(MakeCancelCommand()).code ==
                             PlaybackCancellationJoinCode::kStarted);
  BOOMPI_EXPECT(context, join.ObserveProducerStop(MakeProducerStopAck()).code ==
                             PlaybackCancellationJoinCode::kProgress);
  BOOMPI_EXPECT(context,
                join.ObserveLocalCancel(MakeLocalCancelCompletion()).code ==
                    PlaybackCancellationJoinCode::kProgress);
  BOOMPI_EXPECT(context,
                join.ObserveIngressQuiesced(MakeIngressQuiescedAck()).code ==
                    PlaybackCancellationJoinCode::kProgress);

  auto regressed = MakeIngressQuiescedAck();
  regressed.code = PlaybackIngressQuiesceCode::kInProgress;
  regressed.empty_observed_after_producer_stop = false;
  regressed.reached_iteration_bound = true;
  const auto result = join.ObserveIngressQuiesced(regressed);
  BOOMPI_EXPECT(
      context,
      result.code == PlaybackCancellationJoinCode::kConflictingDuplicate);
  BOOMPI_EXPECT(context,
                result.state == PlaybackCancellationJoinState::kFaulted);
  BOOMPI_EXPECT(context, !result.can_arm_next_generation);
  BOOMPI_EXPECT(context,
                join.ObserveDspReferenceReset(MakeDspResetAck()).code ==
                    PlaybackCancellationJoinCode::kNotActive);
}

void TestBarrierFailuresStayClosed(boompi::test::TestContext& context) {
  {
    PlaybackCancellationJoin join;
    BOOMPI_EXPECT(context, join.Begin(MakeCancelCommand()).code ==
                               PlaybackCancellationJoinCode::kStarted);
    auto failed = MakeProducerStopAck();
    failed.code = TtsProducerStopCode::kFailed;
    failed.producer_stopped = false;
    failed.producer_write_lease_released = false;
    const auto result = join.ObserveProducerStop(failed);
    BOOMPI_EXPECT(context,
                  result.code == PlaybackCancellationJoinCode::kBarrierFailed);
    BOOMPI_EXPECT(context,
                  result.state == PlaybackCancellationJoinState::kFaulted);
    BOOMPI_EXPECT(context, !result.can_arm_next_generation);
  }

  {
    PlaybackCancellationJoin join;
    BOOMPI_EXPECT(context, join.Begin(MakeCancelCommand()).code ==
                               PlaybackCancellationJoinCode::kStarted);
    auto terminal = MakeLocalCancelCompletion();
    terminal.code = PlaybackLocalCancelCode::kRestartRequired;
    terminal.committer_result.code = PlaybackCancelCode::kRestartRequired;
    BOOMPI_EXPECT(context, terminal.valid());
    const auto result = join.ObserveLocalCancel(terminal);
    BOOMPI_EXPECT(
        context,
        result.code == PlaybackCancellationJoinCode::kTerminalRestartRequired);
    BOOMPI_EXPECT(context,
                  result.state == PlaybackCancellationJoinState::kFaulted);
    BOOMPI_EXPECT(context, !result.can_arm_next_generation);
  }

  {
    PlaybackCancellationJoin join;
    BOOMPI_EXPECT(context, join.Begin(MakeCancelCommand()).code ==
                               PlaybackCancellationJoinCode::kStarted);
    BOOMPI_EXPECT(context,
                  join.ObserveProducerStop(MakeProducerStopAck()).code ==
                      PlaybackCancellationJoinCode::kProgress);
    BOOMPI_EXPECT(context,
                  join.ObserveLocalCancel(MakeLocalCancelCompletion()).code ==
                      PlaybackCancellationJoinCode::kProgress);
    BOOMPI_EXPECT(context,
                  join.ObserveIngressQuiesced(MakeIngressQuiescedAck()).code ==
                      PlaybackCancellationJoinCode::kProgress);
    auto failed = MakeDspFailure(DspReferenceResetCode::kBackendFailed);
    failed.accepted_consumer_lease_released = true;
    failed.retired_incarnation_discarded = true;
    failed.accepted_queue_empty_after_local_cancel = true;
    BOOMPI_EXPECT(context, failed.valid());
    const auto result = join.ObserveDspReferenceReset(failed);
    BOOMPI_EXPECT(context,
                  result.code == PlaybackCancellationJoinCode::kBarrierFailed);
    BOOMPI_EXPECT(context, !result.can_arm_next_generation);
  }
}

}  // namespace

int main() {
  boompi::test::TestContext context;
  TestDefaultContractsFailClosed(context);
  TestCommandValidation(context);
  TestPodQueueFifoFullWrapAndPriority(context);
  TestEndpointPriorityLanesRemainIndependent(context);
  TestPodQueueConcurrentFifo(context);
  TestAckValidation(context);
  TestWorkerEndpointProtocols(context);
  TestCancellationJoinHappyPath(context);
  TestCancellationJoinOrderingAndIdentity(context);
  TestAlreadyQuiescentPath(context);
  TestRendererOnlyArmCancelRace(context);
  TestCompletedJoinRejectsEpochBelowAdvancedFence(context);
  TestCompletedTransactionCannotBeReplayed(context);
  TestConflictingDuplicateCannotChangeCancellationMode(context);
  TestWrongIncarnationFailureCannotPoisonTransaction(context);
  TestContradictoryAckAfterReadyRevokesPermit(context);
  TestIngressCannotRegressAfterEmptyAck(context);
  TestBarrierFailuresStayClosed(context);

  if (context.failures() != 0U) {
    std::cerr << context.failures()
              << " playback control expectation(s) failed\n";
    return 1;
  }
  std::cout << "boomPI playback control tests passed\n";
  return 0;
}
