#include "boompi/audio/playback_worker.h"

#include <cstdint>
#include <limits>
#include <type_traits>

namespace boompi::audio {
namespace {

static_assert(std::is_trivially_copyable<PlaybackWorkerStepResult>::value,
              "playback worker step result must remain trivially copyable");
static_assert(std::is_standard_layout<PlaybackWorkerStepResult>::value,
              "playback worker step result must remain standard-layout");

bool IsSuccessfulLocalCancel(const PlaybackCancelResult& result) noexcept {
  return result.acknowledged && result.drop_succeeded &&
         result.prepare_succeeded && result.reference_reset_required &&
         result.retired_pcm_incarnation != 0U &&
         result.prepared_pcm_incarnation != 0U &&
         (result.code == PlaybackCancelCode::kAcknowledged ||
          result.code == PlaybackCancelCode::kDuplicateAcknowledged ||
          result.code == PlaybackCancelCode::kRestartRequired);
}

std::int32_t SafeNativeError(const std::int32_t native_error) noexcept {
  return native_error <= 0 ? native_error : 0;
}

bool AddWithoutOverflow(const std::uint32_t lhs, const std::uint32_t rhs,
                        std::uint32_t* const output) noexcept {
  if (output == nullptr ||
      lhs > std::numeric_limits<std::uint32_t>::max() - rhs) {
    return false;
  }
  *output = lhs + rhs;
  return true;
}

bool LifecycleIdentityIsCorrelatable(
    const PlaybackLifecycleCommand& command) noexcept {
  if (command.request_id == 0U) {
    return false;
  }
  switch (command.type) {
    case PlaybackLifecycleCommandType::kArm:
    case PlaybackLifecycleCommandType::kSetDucked:
    case PlaybackLifecycleCommandType::kQuiesceIngress:
    case PlaybackLifecycleCommandType::kConfirmReferenceReset:
      return command.generation.valid();
    case PlaybackLifecycleCommandType::kShutdown:
      return true;
    case PlaybackLifecycleCommandType::kUnset:
      return false;
  }
  return false;
}

}  // namespace

Status PlaybackWorkerCore::Create(const PlaybackWorkerConfig& config,
                                  const PlaybackWorkerEndpoints& endpoints,
                                  PlaybackWorkerCore* const output) {
  if (output == nullptr) {
    return Status::Error(StatusCode::kInvalidArgument,
                         "playback worker output must not be null");
  }
  if (output->state_ != PlaybackWorkerState::kUnconfigured) {
    return Status::Error(StatusCode::kFailedPrecondition,
                         "playback worker is already configured");
  }
  if (config.first_stream_event_sequence == 0U) {
    return Status::Error(StatusCode::kInvalidArgument,
                         "playback stream event sequence must be non-zero");
  }
  if (endpoints.fence == nullptr || endpoints.sink == nullptr ||
      endpoints.ingress == nullptr || endpoints.urgent_cancel == nullptr ||
      endpoints.lifecycle_commands == nullptr ||
      endpoints.local_cancel_results == nullptr ||
      endpoints.lifecycle_results == nullptr ||
      endpoints.eos_events == nullptr || endpoints.critical_events == nullptr) {
    return Status::Error(StatusCode::kInvalidArgument,
                         "playback worker endpoints must not be null");
  }
  if (!endpoints.fence->is_lock_free()) {
    return Status::Error(StatusCode::kNotSupported,
                         "playback epoch fence must be lock-free");
  }
  if (config.committer.reference_policy ==
          PlaybackReferencePolicy::kRequiredSoftwareReference &&
      endpoints.accepted_queue == nullptr) {
    return Status::Error(
        StatusCode::kInvalidArgument,
        "software playback reference requires an accepted queue");
  }

  // Validate both child configurations on temporary cold-path objects before
  // mutating output. This keeps a failed Create retryable on the same worker.
  PlaybackRenderer24To48 renderer_probe{};
  const Status renderer_probe_status =
      PlaybackRenderer24To48::Create(config.gain_limiter, &renderer_probe);
  if (!renderer_probe_status.ok()) {
    return renderer_probe_status;
  }
  PlaybackCommitter committer_probe{};
  const Status committer_probe_status = PlaybackCommitter::Create(
      config.committer, endpoints.fence, endpoints.sink,
      endpoints.accepted_queue, &committer_probe);
  if (!committer_probe_status.ok()) {
    return committer_probe_status;
  }

  const Status renderer_status =
      PlaybackRenderer24To48::Create(config.gain_limiter, &output->renderer_);
  if (!renderer_status.ok()) {
    return renderer_status;
  }
  const Status committer_status = PlaybackCommitter::Create(
      config.committer, endpoints.fence, endpoints.sink,
      endpoints.accepted_queue, &output->committer_);
  if (!committer_status.ok()) {
    return committer_status;
  }

  output->config_ = config;
  output->endpoints_ = endpoints;
  output->next_stream_event_sequence_ = config.first_stream_event_sequence;
  output->state_ = PlaybackWorkerState::kUninitialized;
  return Status::Ok();
}

Status PlaybackWorkerCore::Initialize() {
  if (state_ == PlaybackWorkerState::kUnconfigured) {
    return Status::Error(StatusCode::kFailedPrecondition,
                         "playback worker is not configured");
  }
  if (state_ != PlaybackWorkerState::kUninitialized) {
    return Status::Error(StatusCode::kFailedPrecondition,
                         "playback worker is already initialized");
  }

  const Status status = committer_.Initialize();
  if (!status.ok()) {
    EnterTerminalFault();
    return status;
  }
  state_ = PlaybackWorkerState::kPreparedIdle;
  return Status::Ok();
}

PlaybackWorkerStepResult PlaybackWorkerCore::Step() noexcept {
  if (state_ == PlaybackWorkerState::kUnconfigured ||
      state_ == PlaybackWorkerState::kUninitialized) {
    return Result(PlaybackWorkerStepCode::kNotInitialized);
  }
  if (state_ == PlaybackWorkerState::kStopped) {
    return Result(PlaybackWorkerStepCode::kStopped);
  }

  // A retained local completion occupies the only local-result slot. The
  // actor cannot lawfully issue another cancel transaction until consuming
  // it; every other pending output remains preemptible by this lane.
  if (!pending_local_cancel_result_valid_) {
    PlaybackCancelCommand cancel{};
    if (endpoints_.urgent_cancel->TryPop(&cancel)) {
      return ProcessUrgentCancel(cancel);
    }
  }

  if (pending_local_cancel_result_valid_ || pending_critical_event_valid_ ||
      pending_eos_event_valid_ || pending_lifecycle_result_valid_) {
    return FlushPendingOutputs();
  }

  if (state_ == PlaybackWorkerState::kCancellationRequired) {
    return Result(PlaybackWorkerStepCode::kAwaitingCancellation);
  }
  if (quiesce_in_progress_) {
    return ContinueIngressQuiescence();
  }
  if (state_ == PlaybackWorkerState::kArming) {
    if (terminal_fault_latched_) {
      return Result(PlaybackWorkerStepCode::kFaulted);
    }
    return ContinueArm();
  }

  PlaybackLifecycleCommand lifecycle{};
  if (endpoints_.lifecycle_commands->TryPop(&lifecycle)) {
    return ProcessLifecycleCommand(lifecycle);
  }

  if (terminal_fault_latched_ || state_ == PlaybackWorkerState::kFaulted) {
    return Result(PlaybackWorkerStepCode::kFaulted);
  }
  if (state_ == PlaybackWorkerState::kActive) {
    return ProcessActiveAudio();
  }
  return Result(PlaybackWorkerStepCode::kIdle);
}

PlaybackWorkerStepResult PlaybackWorkerCore::Result(
    const PlaybackWorkerStepCode code) const noexcept {
  return {code, state()};
}

PlaybackWorkerStepResult PlaybackWorkerCore::ProcessUrgentCancel(
    const PlaybackCancelCommand& command) noexcept {
  if (command.request_id == 0U || !command.generation.valid()) {
    return Result(PlaybackWorkerStepCode::kInvalidControlInput);
  }
  if (cached_local_cancel_result_valid_ && command.valid() &&
      command.request_id == cached_local_cancel_result_.request_id &&
      GenerationsEqual(command.generation,
                       cached_local_cancel_result_.generation) &&
      command.cancel_epoch ==
          cached_local_cancel_result_.observed_fence_epoch) {
    QueueLocalCancelResult(cached_local_cancel_result_);
    return Result(PlaybackWorkerStepCode::kProgress);
  }

  const std::uint32_t observed_epoch = endpoints_.fence->Observe();
  PlaybackLocalCancelCompletion completion{};
  completion.request_id = command.request_id;
  completion.generation = command.generation;
  completion.observed_fence_epoch = observed_epoch;
  completion.worker_holds_no_ingress_lease = true;
  completion.producer_lease_may_publish_late = true;

  if (!command.valid()) {
    completion.code = PlaybackLocalCancelCode::kInvalidCommand;
    QueueLocalCancelResult(completion);
    return Result(PlaybackWorkerStepCode::kProgress);
  }

  const bool exact_active_generation =
      active_generation_.valid() &&
      GenerationsEqual(command.generation, active_generation_);
  const bool fence_exact = observed_epoch == command.cancel_epoch &&
                           observed_epoch > command.generation.epoch;
  if (!fence_exact) {
    if (exact_active_generation && committer_armed_ &&
        observed_epoch <= command.generation.epoch) {
      completion.committer_result =
          committer_.Cancel(command.request_id, command.generation);
      completion.observed_fence_epoch =
          completion.committer_result.observed_cancel_epoch;
      completion.code = PlaybackLocalCancelCode::kFenceNotAdvanced;
    } else {
      completion.code = PlaybackLocalCancelCode::kRejected;
    }
    QueueLocalCancelResult(completion);
    return Result(PlaybackWorkerStepCode::kProgress);
  }

  if (cancel_path_ != CancelPath::kNone) {
    completion.code = PlaybackLocalCancelCode::kRejected;
    QueueLocalCancelResult(completion);
    return Result(PlaybackWorkerStepCode::kProgress);
  }

  const bool renderer_only =
      exact_active_generation && !committer_armed_ &&
      (state_ == PlaybackWorkerState::kArming || renderer_only_cancel_ready_);
  const bool active_sink = exact_active_generation && committer_armed_;
  const bool no_sink =
      state_ == PlaybackWorkerState::kPreparedIdle &&
      !active_generation_.valid() && !renderer_armed_ && !committer_armed_ &&
      command.generation.epoch > highest_seen_generation_epoch_;

  if (!renderer_only && !active_sink && !no_sink) {
    completion.code = PlaybackLocalCancelCode::kGenerationMismatch;
    QueueLocalCancelResult(completion);
    return Result(PlaybackWorkerStepCode::kProgress);
  }

  // Shutdown has not linearized until its ACK is published. Only a validated
  // cancel that starts a real local teardown wins this checkpoint; malformed,
  // mismatched, and exact historical replay messages leave shutdown intact.
  if (pending_lifecycle_result_valid_ &&
      pending_lifecycle_result_.type ==
          PlaybackLifecycleResultType::kShutdown &&
      pending_lifecycle_result_.shutdown.code ==
          PlaybackShutdownResultCode::kAcknowledged) {
    pending_lifecycle_result_.shutdown.code =
        PlaybackShutdownResultCode::kActiveGeneration;
    lifecycle_transition_ = LifecyclePublishTransition::kNone;
  }

  cancellation_command_ = command;
  completion.initial_ingress_drain =
      DrainPublishedTtsFrames(*endpoints_.ingress);

  if (no_sink) {
    highest_seen_generation_epoch_ = command.generation.epoch;
    cancel_path_ = CancelPath::kNoSink;
    cancelled_pcm_incarnation_ = 0U;
    completion.code = PlaybackLocalCancelCode::kAlreadyQuiescent;
    completion.renderer_generation_never_armed = true;
    completion.committer_generation_never_armed = true;
    completion.committer_prepared_idle =
        committer_.state() == PlaybackCommitterState::kPreparedIdle;
    completion.no_accepted_pcm_for_generation = true;
    state_ = PlaybackWorkerState::kAwaitingIngressQuiescence;
    QueueLocalCancelResult(completion);
    return Result(PlaybackWorkerStepCode::kProgress);
  }

  // An unpublished Arm ACK must not escape after cancellation won the race.
  if (pending_lifecycle_result_valid_ &&
      pending_lifecycle_result_.type == PlaybackLifecycleResultType::kArm &&
      GenerationsEqual(pending_lifecycle_result_.arm.generation,
                       command.generation) &&
      pending_lifecycle_result_.arm.code ==
          PlaybackArmResultCode::kAcknowledged) {
    pending_lifecycle_result_.arm.code =
        PlaybackArmResultCode::kCancelledBeforeAcknowledgement;
    pending_lifecycle_result_.arm.observed_fence_epoch = observed_epoch;
    lifecycle_transition_ = LifecyclePublishTransition::kNone;
  }

  // This second Disarm is intentional when the renderer had already stopped
  // itself: it places the explicit local barrier after the observed fence.
  renderer_.Disarm();
  renderer_armed_ = false;
  completion.renderer_disarmed_after_cancel_fence = true;
  ClearPipeline();

  if (renderer_only) {
    if (!pending_lifecycle_result_valid_) {
      PlaybackLifecycleResult arm_result{};
      arm_result.type = PlaybackLifecycleResultType::kArm;
      arm_result.arm.code =
          PlaybackArmResultCode::kCancelledBeforeAcknowledgement;
      arm_result.arm.request_id = arm_command_.request_id;
      arm_result.arm.generation = command.generation;
      arm_result.arm.observed_fence_epoch = observed_epoch;
      QueueLifecycleResult(arm_result);
    }
    cancel_path_ = CancelPath::kRendererOnly;
    cancelled_pcm_incarnation_ = 0U;
    completion.code = PlaybackLocalCancelCode::kRendererOnlyQuiesced;
    completion.committer_generation_never_armed = true;
    completion.committer_prepared_idle =
        committer_.state() == PlaybackCommitterState::kPreparedIdle;
    completion.no_accepted_pcm_for_generation = true;
    renderer_only_cancel_ready_ = false;
    state_ = PlaybackWorkerState::kAwaitingIngressQuiescence;
    QueueLocalCancelResult(completion);
    return Result(PlaybackWorkerStepCode::kProgress);
  }

  completion.committer_result =
      committer_.Cancel(command.request_id, command.generation);
  completion.observed_fence_epoch =
      completion.committer_result.observed_cancel_epoch;
  completion.reference_reset_required =
      completion.committer_result.reference_reset_required;
  cancelled_pcm_incarnation_ =
      completion.committer_result.retired_pcm_incarnation;
  committer_armed_ = false;
  cancel_path_ = CancelPath::kActiveSink;

  if (IsSuccessfulLocalCancel(completion.committer_result)) {
    completion.code =
        completion.committer_result.code == PlaybackCancelCode::kRestartRequired
            ? PlaybackLocalCancelCode::kRestartRequired
            : PlaybackLocalCancelCode::kAcknowledged;
    state_ = completion.code == PlaybackLocalCancelCode::kAcknowledged
                 ? PlaybackWorkerState::kAwaitingIngressQuiescence
                 : PlaybackWorkerState::kCancellationRequired;
    fault_after_local_cancel_publish_ =
        completion.code == PlaybackLocalCancelCode::kRestartRequired;
  } else {
    switch (completion.committer_result.code) {
      case PlaybackCancelCode::kFenceNotAdvanced:
        completion.code = PlaybackLocalCancelCode::kFenceNotAdvanced;
        break;
      case PlaybackCancelCode::kGenerationMismatch:
        completion.code = PlaybackLocalCancelCode::kGenerationMismatch;
        break;
      case PlaybackCancelCode::kInvalidRequest:
        completion.code = PlaybackLocalCancelCode::kInvalidCommand;
        break;
      case PlaybackCancelCode::kRestartRequired:
      case PlaybackCancelCode::kIncarnationExhausted:
        completion.code = PlaybackLocalCancelCode::kRestartRequired;
        break;
      case PlaybackCancelCode::kUnset:
      case PlaybackCancelCode::kAcknowledged:
      case PlaybackCancelCode::kDuplicateAcknowledged:
      case PlaybackCancelCode::kNotConfigured:
      case PlaybackCancelCode::kNotActive:
      case PlaybackCancelCode::kDropFailed:
      case PlaybackCancelCode::kPrepareFailed:
      case PlaybackCancelCode::kSinkFault:
        completion.code = PlaybackLocalCancelCode::kRejected;
        break;
    }
    state_ = PlaybackWorkerState::kCancellationRequired;
    fault_after_local_cancel_publish_ = true;
  }

  QueueLocalCancelResult(completion);
  return Result(PlaybackWorkerStepCode::kProgress);
}

PlaybackWorkerStepResult PlaybackWorkerCore::FlushPendingOutputs() noexcept {
  if (pending_local_cancel_result_valid_) {
    if (!endpoints_.local_cancel_results->TryPush(
            pending_local_cancel_result_)) {
      return Result(PlaybackWorkerStepCode::kOutputBackpressure);
    }
    pending_local_cancel_result_valid_ = false;
    if (fault_after_local_cancel_publish_) {
      fault_after_local_cancel_publish_ = false;
      EnterTerminalFault();
    }
    return Result(PlaybackWorkerStepCode::kProgress);
  }
  if (pending_critical_event_valid_) {
    if (!endpoints_.critical_events->TryPush(pending_critical_event_)) {
      return Result(PlaybackWorkerStepCode::kOutputBackpressure);
    }
    pending_critical_event_valid_ = false;
    return Result(PlaybackWorkerStepCode::kProgress);
  }
  if (pending_eos_event_valid_) {
    if (!endpoints_.eos_events->TryPush(pending_eos_event_)) {
      return Result(PlaybackWorkerStepCode::kOutputBackpressure);
    }
    pending_eos_event_valid_ = false;
    return Result(PlaybackWorkerStepCode::kProgress);
  }
  if (pending_lifecycle_result_valid_) {
    if (!endpoints_.lifecycle_results->TryPush(pending_lifecycle_result_)) {
      return Result(PlaybackWorkerStepCode::kOutputBackpressure);
    }
    pending_lifecycle_result_valid_ = false;
    ApplyLifecyclePublishTransition();
    return Result(PlaybackWorkerStepCode::kProgress);
  }
  return Result(PlaybackWorkerStepCode::kIdle);
}

PlaybackWorkerStepResult PlaybackWorkerCore::ProcessLifecycleCommand(
    const PlaybackLifecycleCommand& command) noexcept {
  // Every public result type requires a reply-correlatable identity. Never
  // fabricate a result whose own valid() rejects the echoed envelope.
  if (!LifecycleIdentityIsCorrelatable(command)) {
    return Result(PlaybackWorkerStepCode::kInvalidControlInput);
  }
  switch (command.type) {
    case PlaybackLifecycleCommandType::kArm: {
      PlaybackLifecycleResult result{};
      result.type = PlaybackLifecycleResultType::kArm;
      result.arm.request_id = command.request_id;
      result.arm.generation = command.generation;
      result.arm.observed_fence_epoch = endpoints_.fence->Observe();
      if (!command.valid()) {
        result.arm.code = PlaybackArmResultCode::kInvalidCommand;
        QueueLifecycleResult(result);
        return Result(PlaybackWorkerStepCode::kProgress);
      }
      if (terminal_fault_latched_) {
        result.arm.code = PlaybackArmResultCode::kFaulted;
        QueueLifecycleResult(result);
        return Result(PlaybackWorkerStepCode::kFaulted);
      }
      if (state_ != PlaybackWorkerState::kPreparedIdle ||
          event_sequence_exhausted_) {
        result.arm.code = event_sequence_exhausted_
                              ? PlaybackArmResultCode::kFaulted
                              : PlaybackArmResultCode::kCommitterRejected;
        QueueLifecycleResult(result);
        if (event_sequence_exhausted_) {
          EnterTerminalFault();
        }
        return Result(PlaybackWorkerStepCode::kProgress);
      }
      if (result.arm.observed_fence_epoch != command.generation.epoch) {
        result.arm.code = PlaybackArmResultCode::kFenceMismatch;
        QueueLifecycleResult(result);
        return Result(PlaybackWorkerStepCode::kProgress);
      }

      if (command.generation.epoch > highest_seen_generation_epoch_) {
        highest_seen_generation_epoch_ = command.generation.epoch;
      }
      const Status arm_status = renderer_.Arm(command.generation);
      if (!arm_status.ok()) {
        result.arm.code = PlaybackArmResultCode::kRendererRejected;
        QueueLifecycleResult(result);
        return Result(PlaybackWorkerStepCode::kProgress);
      }
      active_generation_ = command.generation;
      arm_command_ = command;
      renderer_armed_ = true;
      committer_armed_ = false;
      renderer_only_cancel_ready_ = false;
      state_ = PlaybackWorkerState::kArming;
      return Result(PlaybackWorkerStepCode::kProgress);
    }

    case PlaybackLifecycleCommandType::kSetDucked: {
      PlaybackLifecycleResult result{};
      result.type = PlaybackLifecycleResultType::kDuck;
      result.duck.request_id = command.request_id;
      result.duck.generation = command.generation;
      result.duck.ducked = command.value;
      if (!command.valid()) {
        result.duck.code = PlaybackDuckResultCode::kInvalidCommand;
      } else if (!active_generation_.valid() ||
                 state_ != PlaybackWorkerState::kActive || !renderer_armed_) {
        result.duck.code = PlaybackDuckResultCode::kNotArmed;
      } else if (!GenerationsEqual(command.generation, active_generation_)) {
        result.duck.code = PlaybackDuckResultCode::kGenerationMismatch;
      } else if (renderer_drain_pending_) {
        // Process(EOS) has closed duck control until the exact FIR tail is
        // drained. Reject without calling a stage that is known to reject.
        result.duck.code = PlaybackDuckResultCode::kNotArmed;
      } else {
        const Status duck_status = renderer_.SetDucked(command.value);
        result.duck.code = duck_status.ok()
                               ? PlaybackDuckResultCode::kAcknowledged
                               : PlaybackDuckResultCode::kRendererRejected;
        if (!duck_status.ok()) {
          RequireCancellation(0);
        }
      }
      QueueLifecycleResult(result);
      return Result(PlaybackWorkerStepCode::kProgress);
    }

    case PlaybackLifecycleCommandType::kQuiesceIngress: {
      const bool exact_acknowledged_replay =
          last_acknowledged_quiesce_valid_ && command.valid() &&
          command.request_id == last_acknowledged_quiesce_command_.request_id &&
          GenerationsEqual(command.generation,
                           last_acknowledged_quiesce_command_.generation) &&
          command.retired_pcm_incarnation ==
              last_acknowledged_quiesce_command_.retired_pcm_incarnation &&
          command.value == last_acknowledged_quiesce_command_.value;
      if (exact_acknowledged_replay) {
        QueueLifecycleResult(last_acknowledged_quiesce_result_);
        return Result(PlaybackWorkerStepCode::kProgress);
      }
      if (!command.valid() || !command.value) {
        PlaybackLifecycleResult result{};
        result.type = PlaybackLifecycleResultType::kIngressQuiesced;
        result.ingress_quiesced.code =
            command.value ? PlaybackIngressQuiesceCode::kInvalidCommand
                          : PlaybackIngressQuiesceCode::kProducerNotStopped;
        result.ingress_quiesced.request_id = command.request_id;
        result.ingress_quiesced.generation = command.generation;
        result.ingress_quiesced.retired_pcm_incarnation =
            command.retired_pcm_incarnation;
        QueueLifecycleResult(result);
        return Result(PlaybackWorkerStepCode::kProgress);
      }
      if (cancel_path_ == CancelPath::kNone ||
          command.request_id != cancellation_command_.request_id ||
          !GenerationsEqual(command.generation,
                            cancellation_command_.generation)) {
        PlaybackLifecycleResult result{};
        result.type = PlaybackLifecycleResultType::kIngressQuiesced;
        result.ingress_quiesced.code =
            PlaybackIngressQuiesceCode::kGenerationMismatch;
        result.ingress_quiesced.request_id = command.request_id;
        result.ingress_quiesced.generation = command.generation;
        result.ingress_quiesced.retired_pcm_incarnation =
            command.retired_pcm_incarnation;
        result.ingress_quiesced.producer_stop_precondition = true;
        QueueLifecycleResult(result);
        return Result(PlaybackWorkerStepCode::kProgress);
      }
      if (command.retired_pcm_incarnation != cancelled_pcm_incarnation_) {
        PlaybackLifecycleResult result{};
        result.type = PlaybackLifecycleResultType::kIngressQuiesced;
        result.ingress_quiesced.code =
            PlaybackIngressQuiesceCode::kIncarnationMismatch;
        result.ingress_quiesced.request_id = command.request_id;
        result.ingress_quiesced.generation = command.generation;
        result.ingress_quiesced.retired_pcm_incarnation =
            command.retired_pcm_incarnation;
        result.ingress_quiesced.producer_stop_precondition = true;
        QueueLifecycleResult(result);
        return Result(PlaybackWorkerStepCode::kProgress);
      }

      quiesce_command_ = command;
      quiesce_discarded_frames_ = 0U;
      quiesce_discarded_source_samples_ = 0U;
      quiesce_in_progress_ = true;
      state_ = PlaybackWorkerState::kAwaitingIngressQuiescence;
      return Result(PlaybackWorkerStepCode::kProgress);
    }

    case PlaybackLifecycleCommandType::kConfirmReferenceReset: {
      PlaybackLifecycleResult result{};
      result.type = PlaybackLifecycleResultType::kReferenceReset;
      const bool current_confirmation =
          cancel_path_ == CancelPath::kActiveSink &&
          state_ == PlaybackWorkerState::kAwaitingReferenceReset &&
          command.retired_pcm_incarnation == cancelled_pcm_incarnation_;
      const bool cached_confirmed_identity =
          last_confirmed_reference_reset_valid_ &&
          command.request_id ==
              last_confirmed_reference_reset_command_.request_id &&
          GenerationsEqual(
              command.generation,
              last_confirmed_reference_reset_command_.generation) &&
          command.retired_pcm_incarnation ==
              last_confirmed_reference_reset_command_.retired_pcm_incarnation;
      if (!command.valid()) {
        result.reference_reset = {
            PlaybackReferenceResetCode::kInvalidConfirmation,
            command.request_id, command.generation, false};
      } else if (!current_confirmation && !cached_confirmed_identity) {
        result.reference_reset = {PlaybackReferenceResetCode::kNotAwaiting,
                                  command.request_id, command.generation,
                                  false};
      } else {
        result.reference_reset = committer_.ConfirmReferenceReset(
            command.request_id, command.generation);
      }
      const bool completes_current_confirmation =
          current_confirmation && result.reference_reset.confirmed;
      if (completes_current_confirmation) {
        pending_reference_reset_command_ = command;
      }
      QueueLifecycleResult(result,
                           completes_current_confirmation
                               ? LifecyclePublishTransition::kPreparedIdle
                               : LifecyclePublishTransition::kNone);
      return Result(PlaybackWorkerStepCode::kProgress);
    }

    case PlaybackLifecycleCommandType::kShutdown: {
      PlaybackLifecycleResult result{};
      result.type = PlaybackLifecycleResultType::kShutdown;
      result.shutdown.request_id = command.request_id;
      if (!command.valid()) {
        result.shutdown.code = PlaybackShutdownResultCode::kInvalidCommand;
      } else if (terminal_fault_latched_) {
        result.shutdown.code = PlaybackShutdownResultCode::kFaulted;
      } else if (!IsCompletelyIdle(state_) || active_generation_.valid() ||
                 renderer_armed_ || committer_armed_ ||
                 cancel_path_ != CancelPath::kNone || rendered_ready_ ||
                 committer_chunk_pending_ || renderer_drain_pending_) {
        result.shutdown.code = PlaybackShutdownResultCode::kActiveGeneration;
      } else {
        result.shutdown.code = PlaybackShutdownResultCode::kAcknowledged;
      }
      QueueLifecycleResult(result, result.shutdown.acknowledged()
                                       ? LifecyclePublishTransition::kStopped
                                       : LifecyclePublishTransition::kNone);
      return Result(PlaybackWorkerStepCode::kProgress);
    }

    case PlaybackLifecycleCommandType::kUnset:
      return Result(PlaybackWorkerStepCode::kInvalidControlInput);
  }

  EnterTerminalFault();
  return Result(PlaybackWorkerStepCode::kFaulted);
}

PlaybackWorkerStepResult PlaybackWorkerCore::ContinueArm() noexcept {
  const std::uint32_t observed_epoch = endpoints_.fence->Observe();
  PlaybackLifecycleResult result{};
  result.type = PlaybackLifecycleResultType::kArm;
  result.arm.request_id = arm_command_.request_id;
  result.arm.generation = arm_command_.generation;
  result.arm.observed_fence_epoch = observed_epoch;

  if (observed_epoch != active_generation_.epoch) {
    renderer_.Disarm();
    renderer_armed_ = false;
    renderer_only_cancel_ready_ = true;
    result.arm.code = PlaybackArmResultCode::kCancelledBeforeAcknowledgement;
    state_ = PlaybackWorkerState::kCancellationRequired;
    QueueLifecycleResult(result);
    return Result(PlaybackWorkerStepCode::kProgress);
  }

  const Status arm_status = committer_.Arm(active_generation_);
  const std::uint32_t confirmed_epoch = endpoints_.fence->Observe();
  result.arm.observed_fence_epoch = confirmed_epoch;
  if (!arm_status.ok()) {
    renderer_.Disarm();
    renderer_armed_ = false;
    if (confirmed_epoch != active_generation_.epoch) {
      renderer_only_cancel_ready_ = true;
      result.arm.code = PlaybackArmResultCode::kCancelledBeforeAcknowledgement;
      state_ = PlaybackWorkerState::kCancellationRequired;
    } else {
      result.arm.code = PlaybackArmResultCode::kCommitterRejected;
      active_generation_ = PlaybackGeneration{};
      renderer_only_cancel_ready_ = false;
      state_ = PlaybackWorkerState::kPreparedIdle;
    }
    QueueLifecycleResult(result);
    return Result(PlaybackWorkerStepCode::kProgress);
  }

  committer_armed_ = true;
  accepted_generation_sample_frames_ = 0U;
  last_accepted_chunk_sequence_ = 0U;
  if (confirmed_epoch != active_generation_.epoch) {
    result.arm.code = PlaybackArmResultCode::kCancelledBeforeAcknowledgement;
    state_ = PlaybackWorkerState::kCancellationRequired;
  } else {
    result.arm.code = PlaybackArmResultCode::kAcknowledged;
  }
  QueueLifecycleResult(result, result.arm.acknowledged()
                                   ? LifecyclePublishTransition::kActive
                                   : LifecyclePublishTransition::kNone);
  return Result(PlaybackWorkerStepCode::kProgress);
}

PlaybackWorkerStepResult
PlaybackWorkerCore::ContinueIngressQuiescence() noexcept {
  const PlaybackDrainResult drain =
      DrainPublishedTtsFrames(*endpoints_.ingress);
  std::uint32_t discarded_frames = 0U;
  std::uint32_t discarded_samples = 0U;
  if (!AddWithoutOverflow(quiesce_discarded_frames_, drain.discarded_frames,
                          &discarded_frames) ||
      !AddWithoutOverflow(quiesce_discarded_source_samples_,
                          drain.discarded_source_samples, &discarded_samples)) {
    PlaybackLifecycleResult result{};
    result.type = PlaybackLifecycleResultType::kIngressQuiesced;
    result.ingress_quiesced.code = PlaybackIngressQuiesceCode::kFaulted;
    result.ingress_quiesced.request_id = quiesce_command_.request_id;
    result.ingress_quiesced.generation = quiesce_command_.generation;
    result.ingress_quiesced.retired_pcm_incarnation =
        quiesce_command_.retired_pcm_incarnation;
    result.ingress_quiesced.discarded_frames = quiesce_discarded_frames_;
    result.ingress_quiesced.discarded_source_samples =
        quiesce_discarded_source_samples_;
    result.ingress_quiesced.producer_stop_precondition = true;
    quiesce_in_progress_ = false;
    QueueLifecycleResult(result);
    EnterTerminalFault();
    return Result(PlaybackWorkerStepCode::kFaulted);
  }
  quiesce_discarded_frames_ = discarded_frames;
  quiesce_discarded_source_samples_ = discarded_samples;

  PlaybackLifecycleResult result{};
  result.type = PlaybackLifecycleResultType::kIngressQuiesced;
  result.ingress_quiesced.request_id = quiesce_command_.request_id;
  result.ingress_quiesced.generation = quiesce_command_.generation;
  result.ingress_quiesced.retired_pcm_incarnation =
      quiesce_command_.retired_pcm_incarnation;
  result.ingress_quiesced.discarded_frames = quiesce_discarded_frames_;
  result.ingress_quiesced.discarded_source_samples =
      quiesce_discarded_source_samples_;
  result.ingress_quiesced.producer_stop_precondition = true;
  result.ingress_quiesced.reached_iteration_bound =
      drain.reached_iteration_bound;

  if (drain.reached_iteration_bound) {
    result.ingress_quiesced.code = PlaybackIngressQuiesceCode::kInProgress;
    QueueLifecycleResult(result);
    return Result(PlaybackWorkerStepCode::kProgress);
  }

  result.ingress_quiesced.code = PlaybackIngressQuiesceCode::kAcknowledged;
  result.ingress_quiesced.empty_observed_after_producer_stop = true;
  quiesce_in_progress_ = false;
  last_acknowledged_quiesce_command_ = quiesce_command_;
  last_acknowledged_quiesce_result_ = result;
  last_acknowledged_quiesce_valid_ = true;
  QueueLifecycleResult(result,
                       cancel_path_ == CancelPath::kActiveSink
                           ? LifecyclePublishTransition::kAwaitingReferenceReset
                           : LifecyclePublishTransition::kPreparedIdle);
  return Result(PlaybackWorkerStepCode::kProgress);
}

PlaybackWorkerStepResult PlaybackWorkerCore::ProcessActiveAudio() noexcept {
  if (endpoints_.fence->Observe() != active_generation_.epoch) {
    state_ = PlaybackWorkerState::kCancellationRequired;
    return Result(PlaybackWorkerStepCode::kAwaitingCancellation);
  }
  if (committer_chunk_pending_) {
    return PumpCommitter();
  }
  if (rendered_ready_) {
    return SubmitRenderedFrame();
  }
  if (renderer_drain_pending_) {
    return DrainRenderer();
  }
  return RenderIngressFrame();
}

PlaybackWorkerStepResult PlaybackWorkerCore::SubmitRenderedFrame() noexcept {
  if (endpoints_.fence->Observe() != active_generation_.epoch) {
    state_ = PlaybackWorkerState::kCancellationRequired;
    return Result(PlaybackWorkerStepCode::kAwaitingCancellation);
  }

  const PlaybackSubmitResult submit = committer_.Submit(rendered_frame_);
  if (submit.accepted()) {
    rendered_ready_ = false;
    committer_chunk_pending_ = true;
    return Result(PlaybackWorkerStepCode::kProgress);
  }

  RequireCancellation(0);
  return Result(PlaybackWorkerStepCode::kAwaitingCancellation);
}

PlaybackWorkerStepResult PlaybackWorkerCore::PumpCommitter() noexcept {
  const PlaybackCommitResult commit = committer_.PumpOnce();
  accepted_generation_sample_frames_ = commit.generation_accepted_sample_frames;
  last_accepted_chunk_sequence_ = commit.accepted_chunk_sequence;

  switch (commit.code) {
    case PlaybackCommitCode::kAcceptedPartial:
      return Result(PlaybackWorkerStepCode::kProgress);
    case PlaybackCommitCode::kChunkAccepted:
      committer_chunk_pending_ = false;
      return Result(PlaybackWorkerStepCode::kProgress);
    case PlaybackCommitCode::kEndOfStreamAccepted:
      committer_chunk_pending_ = false;
      if (!QueueEndOfStreamEvent(commit)) {
        EnterTerminalFault();
        return Result(PlaybackWorkerStepCode::kFaulted);
      }
      state_ = PlaybackWorkerState::kEndOfStreamAccepted;
      return Result(PlaybackWorkerStepCode::kProgress);
    case PlaybackCommitCode::kWouldBlock:
    case PlaybackCommitCode::kInterrupted:
    case PlaybackCommitCode::kReferenceBackpressure:
      return Result(PlaybackWorkerStepCode::kSinkBackpressure);
    case PlaybackCommitCode::kUnset:
    case PlaybackCommitCode::kNotConfigured:
    case PlaybackCommitCode::kNotArmed:
    case PlaybackCommitCode::kNoChunkPending:
    case PlaybackCommitCode::kEndOfStreamAlreadyAccepted:
    case PlaybackCommitCode::kCancellationRequiredBeforeWrite:
    case PlaybackCommitCode::kAcceptedThenCancellationRequired:
    case PlaybackCommitCode::kAcceptedThenSinkProtocolFault:
    case PlaybackCommitCode::kXrun:
    case PlaybackCommitCode::kSuspended:
    case PlaybackCommitCode::kDeviceLost:
    case PlaybackCommitCode::kZeroProgress:
    case PlaybackCommitCode::kSinkProtocolFault:
    case PlaybackCommitCode::kReferencePublishFault:
    case PlaybackCommitCode::kRestartRequired:
    case PlaybackCommitCode::kSinkFault:
      RequireCancellation(commit.native_error);
      return Result(PlaybackWorkerStepCode::kAwaitingCancellation);
  }

  EnterTerminalFault();
  return Result(PlaybackWorkerStepCode::kFaulted);
}

PlaybackWorkerStepResult PlaybackWorkerCore::RenderIngressFrame() noexcept {
  auto lease = endpoints_.ingress->TryAcquireRead();
  if (!lease) {
    return Result(PlaybackWorkerStepCode::kIdle);
  }
  if (endpoints_.fence->Observe() != active_generation_.epoch) {
    (void)lease.Release();
    state_ = PlaybackWorkerState::kCancellationRequired;
    return Result(PlaybackWorkerStepCode::kAwaitingCancellation);
  }

  const PlaybackRenderResult render =
      renderer_.Process(lease.frame(), &rendered_frame_);
  const bool released = lease.Release();
  if (!released) {
    EnterTerminalFault();
    return Result(PlaybackWorkerStepCode::kFaulted);
  }
  if (render.produced()) {
    rendered_ready_ = true;
    renderer_drain_pending_ = render.drain_required;
    return Result(PlaybackWorkerStepCode::kProgress);
  }

  switch (render.code) {
    case PlaybackRenderCode::kInvalidGeneration:
    case PlaybackRenderCode::kEpochMismatch:
    case PlaybackRenderCode::kStreamMismatch:
    case PlaybackRenderCode::kTurnMismatch:
      // Stale producer data is safely discarded before any sink boundary.
      return Result(PlaybackWorkerStepCode::kProgress);
    case PlaybackRenderCode::kProduced:
      break;
    case PlaybackRenderCode::kNotConfigured:
    case PlaybackRenderCode::kInvalidOutput:
    case PlaybackRenderCode::kNotArmed:
    case PlaybackRenderCode::kDrainPending:
    case PlaybackRenderCode::kNoDrainPending:
    case PlaybackRenderCode::kInvalidInputFrame:
    case PlaybackRenderCode::kDiscontinuity:
    case PlaybackRenderCode::kSequenceBreak:
    case PlaybackRenderCode::kTimestampNotIncreasing:
    case PlaybackRenderCode::kStageFault:
    case PlaybackRenderCode::kFaulted:
      RequireCancellation(0);
      return Result(PlaybackWorkerStepCode::kAwaitingCancellation);
  }

  EnterTerminalFault();
  return Result(PlaybackWorkerStepCode::kFaulted);
}

PlaybackWorkerStepResult PlaybackWorkerCore::DrainRenderer() noexcept {
  if (endpoints_.fence->Observe() != active_generation_.epoch) {
    state_ = PlaybackWorkerState::kCancellationRequired;
    return Result(PlaybackWorkerStepCode::kAwaitingCancellation);
  }
  const PlaybackRenderResult render = renderer_.Drain(&rendered_frame_);
  if (!render.produced()) {
    RequireCancellation(0);
    return Result(PlaybackWorkerStepCode::kAwaitingCancellation);
  }
  renderer_armed_ = false;
  renderer_drain_pending_ = false;
  rendered_ready_ = true;
  return Result(PlaybackWorkerStepCode::kProgress);
}

void PlaybackWorkerCore::QueueLifecycleResult(
    const PlaybackLifecycleResult& result,
    const LifecyclePublishTransition transition) noexcept {
  if (pending_lifecycle_result_valid_) {
    EnterTerminalFault();
    return;
  }
  pending_lifecycle_result_ = result;
  pending_lifecycle_result_valid_ = true;
  lifecycle_transition_ = transition;
}

void PlaybackWorkerCore::QueueLocalCancelResult(
    const PlaybackLocalCancelCompletion& result) noexcept {
  if (pending_local_cancel_result_valid_) {
    EnterTerminalFault();
    return;
  }
  pending_local_cancel_result_ = result;
  pending_local_cancel_result_valid_ = true;
  switch (result.code) {
    case PlaybackLocalCancelCode::kAcknowledged:
    case PlaybackLocalCancelCode::kAlreadyQuiescent:
    case PlaybackLocalCancelCode::kRendererOnlyQuiesced:
    case PlaybackLocalCancelCode::kRestartRequired:
      cached_local_cancel_result_ = result;
      cached_local_cancel_result_valid_ = true;
      break;
    case PlaybackLocalCancelCode::kUnset:
    case PlaybackLocalCancelCode::kInvalidCommand:
    case PlaybackLocalCancelCode::kGenerationMismatch:
    case PlaybackLocalCancelCode::kFenceNotAdvanced:
    case PlaybackLocalCancelCode::kRejected:
    case PlaybackLocalCancelCode::kFaulted:
      break;
  }
}

bool PlaybackWorkerCore::QueueCriticalEvent(
    const std::int32_t native_error) noexcept {
  if (pending_critical_event_valid_) {
    return true;
  }
  std::uint64_t sequence = 0U;
  if (!AllocateEventSequence(&sequence) || !active_generation_.valid() ||
      committer_.pcm_incarnation() == 0U) {
    return false;
  }
  pending_critical_event_ = PlaybackCriticalStreamEvent{};
  pending_critical_event_.code =
      PlaybackCriticalStreamEventCode::kCancellationRequired;
  pending_critical_event_.event_sequence = sequence;
  pending_critical_event_.generation = active_generation_;
  pending_critical_event_.pcm_incarnation = committer_.pcm_incarnation();
  pending_critical_event_.accepted_generation_sample_frames =
      accepted_generation_sample_frames_;
  pending_critical_event_.last_accepted_chunk_sequence =
      last_accepted_chunk_sequence_;
  pending_critical_event_.native_error = SafeNativeError(native_error);
  pending_critical_event_valid_ = true;
  return true;
}

bool PlaybackWorkerCore::QueueEndOfStreamEvent(
    const PlaybackCommitResult& commit_result) noexcept {
  if (pending_eos_event_valid_) {
    return true;
  }
  std::uint64_t sequence = 0U;
  if (!AllocateEventSequence(&sequence)) {
    return false;
  }
  pending_eos_event_ = PlaybackEndOfStreamAcceptedEvent{};
  pending_eos_event_.event_sequence = sequence;
  pending_eos_event_.generation = active_generation_;
  pending_eos_event_.pcm_incarnation = committer_.pcm_incarnation();
  pending_eos_event_.accepted_generation_sample_frames =
      commit_result.generation_accepted_sample_frames;
  pending_eos_event_.last_accepted_chunk_sequence =
      commit_result.accepted_chunk_sequence;
  pending_eos_event_valid_ = true;
  return pending_eos_event_.valid();
}

bool PlaybackWorkerCore::AllocateEventSequence(
    std::uint64_t* const sequence) noexcept {
  if (sequence == nullptr || event_sequence_exhausted_ ||
      next_stream_event_sequence_ == 0U) {
    return false;
  }
  *sequence = next_stream_event_sequence_;
  if (next_stream_event_sequence_ ==
      std::numeric_limits<std::uint64_t>::max()) {
    event_sequence_exhausted_ = true;
    next_stream_event_sequence_ = 0U;
  } else {
    ++next_stream_event_sequence_;
  }
  return true;
}

void PlaybackWorkerCore::RequireCancellation(
    const std::int32_t native_error) noexcept {
  rendered_ready_ = false;
  renderer_drain_pending_ = false;
  rendered_frame_.ResetHeader();
  if (renderer_armed_) {
    renderer_.Disarm();
    renderer_armed_ = false;
  }
  state_ = PlaybackWorkerState::kCancellationRequired;
  if (!QueueCriticalEvent(native_error)) {
    EnterTerminalFault();
  }
}

void PlaybackWorkerCore::ApplyLifecyclePublishTransition() noexcept {
  const LifecyclePublishTransition transition = lifecycle_transition_;
  lifecycle_transition_ = LifecyclePublishTransition::kNone;
  if (transition == LifecyclePublishTransition::kPreparedIdle &&
      pending_lifecycle_result_.type ==
          PlaybackLifecycleResultType::kReferenceReset &&
      pending_lifecycle_result_.reference_reset.confirmed &&
      pending_reference_reset_command_.valid()) {
    last_confirmed_reference_reset_command_ = pending_reference_reset_command_;
    last_confirmed_reference_reset_valid_ = true;
  }
  pending_reference_reset_command_ = PlaybackLifecycleCommand{};
  switch (transition) {
    case LifecyclePublishTransition::kNone:
      return;
    case LifecyclePublishTransition::kActive:
      state_ = terminal_fault_latched_ ? PlaybackWorkerState::kFaulted
                                       : PlaybackWorkerState::kActive;
      return;
    case LifecyclePublishTransition::kPreparedIdle:
      renderer_.Disarm();
      renderer_armed_ = false;
      committer_armed_ = false;
      active_generation_ = PlaybackGeneration{};
      ClearPipeline();
      ClearCancellationContext();
      if (event_sequence_exhausted_) {
        terminal_fault_latched_ = true;
      }
      state_ = terminal_fault_latched_ ? PlaybackWorkerState::kFaulted
                                       : PlaybackWorkerState::kPreparedIdle;
      return;
    case LifecyclePublishTransition::kAwaitingReferenceReset:
      state_ = PlaybackWorkerState::kAwaitingReferenceReset;
      return;
    case LifecyclePublishTransition::kStopped:
      state_ = PlaybackWorkerState::kStopped;
      return;
  }
  EnterTerminalFault();
}

void PlaybackWorkerCore::ClearPipeline() noexcept {
  rendered_frame_.ResetHeader();
  rendered_ready_ = false;
  committer_chunk_pending_ = false;
  renderer_drain_pending_ = false;
}

void PlaybackWorkerCore::ClearCancellationContext() noexcept {
  cancellation_command_ = PlaybackCancelCommand{};
  cancelled_pcm_incarnation_ = 0U;
  cancel_path_ = CancelPath::kNone;
  renderer_only_cancel_ready_ = false;
  quiesce_command_ = PlaybackLifecycleCommand{};
  quiesce_discarded_frames_ = 0U;
  quiesce_discarded_source_samples_ = 0U;
  quiesce_in_progress_ = false;
}

void PlaybackWorkerCore::EnterTerminalFault() noexcept {
  terminal_fault_latched_ = true;
  if (state_ != PlaybackWorkerState::kStopped) {
    state_ = PlaybackWorkerState::kFaulted;
  }
}

bool PlaybackWorkerCore::GenerationsEqual(
    const PlaybackGeneration& lhs, const PlaybackGeneration& rhs) noexcept {
  return lhs.epoch == rhs.epoch && lhs.turn_id == rhs.turn_id &&
         lhs.stream_id == rhs.stream_id;
}

bool PlaybackWorkerCore::IsCompletelyIdle(
    const PlaybackWorkerState state) noexcept {
  return state == PlaybackWorkerState::kPreparedIdle;
}

}  // namespace boompi::audio
