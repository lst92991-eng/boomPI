#include "boompi/audio/playback_control.h"

#include <limits>
#include <type_traits>

namespace boompi::audio {
namespace {

static_assert(std::is_trivially_copyable<PlaybackLifecycleCommand>::value,
              "playback lifecycle command must remain trivially copyable");
static_assert(std::is_standard_layout<PlaybackLifecycleCommand>::value,
              "playback lifecycle command must remain standard-layout");
static_assert(std::is_trivially_copyable<PlaybackCancelCommand>::value,
              "playback cancel command must remain trivially copyable");
static_assert(std::is_standard_layout<PlaybackCancelCommand>::value,
              "playback cancel command must remain standard-layout");
static_assert(std::is_trivially_copyable<PlaybackLifecycleResult>::value,
              "playback lifecycle result must remain trivially copyable");
static_assert(std::is_standard_layout<PlaybackLifecycleResult>::value,
              "playback lifecycle result must remain standard-layout");
static_assert(
    std::is_trivially_copyable<PlaybackLocalCancelCompletion>::value,
    "playback local cancel completion must remain trivially copyable");
static_assert(std::is_standard_layout<PlaybackLocalCancelCompletion>::value,
              "playback local cancel completion must remain standard-layout");
static_assert(std::is_trivially_copyable<TtsProducerStopAck>::value,
              "TTS producer stop ACK must remain trivially copyable");
static_assert(std::is_standard_layout<TtsProducerStopAck>::value,
              "TTS producer stop ACK must remain standard-layout");
static_assert(std::is_trivially_copyable<DspReferenceResetAck>::value,
              "DSP reference reset ACK must remain trivially copyable");
static_assert(std::is_standard_layout<DspReferenceResetAck>::value,
              "DSP reference reset ACK must remain standard-layout");
static_assert(std::is_trivially_copyable<TtsProducerStartPermit>::value,
              "TTS producer start permit must remain trivially copyable");
static_assert(std::is_standard_layout<TtsProducerStartPermit>::value,
              "TTS producer start permit must remain standard-layout");
static_assert(std::is_trivially_copyable<TtsProducerStopRequest>::value,
              "TTS producer stop request must remain trivially copyable");
static_assert(std::is_standard_layout<TtsProducerStopRequest>::value,
              "TTS producer stop request must remain standard-layout");
static_assert(std::is_trivially_copyable<DspReferenceResetRequest>::value,
              "DSP reset request must remain trivially copyable");
static_assert(std::is_standard_layout<DspReferenceResetRequest>::value,
              "DSP reset request must remain standard-layout");
static_assert(
    std::is_trivially_copyable<PlaybackEndOfStreamAcceptedEvent>::value,
    "playback EOS event must remain trivially copyable");
static_assert(std::is_standard_layout<PlaybackEndOfStreamAcceptedEvent>::value,
              "playback EOS event must remain standard-layout");
static_assert(std::is_trivially_copyable<PlaybackCriticalStreamEvent>::value,
              "critical playback event must remain trivially copyable");
static_assert(std::is_standard_layout<PlaybackCriticalStreamEvent>::value,
              "critical playback event must remain standard-layout");

bool GenerationsEqual(const PlaybackGeneration& lhs,
                      const PlaybackGeneration& rhs) noexcept {
  return lhs.epoch == rhs.epoch && lhs.turn_id == rhs.turn_id &&
         lhs.stream_id == rhs.stream_id;
}

bool GenerationIsZero(const PlaybackGeneration& generation) noexcept {
  return generation.epoch == 0U && generation.turn_id == 0U &&
         generation.stream_id == 0U;
}

bool ReferenceResetResultValid(
    const PlaybackReferenceResetResult& result) noexcept {
  if (result.request_id == 0U || !result.generation.valid()) {
    return false;
  }
  switch (result.code) {
    case PlaybackReferenceResetCode::kConfirmed:
    case PlaybackReferenceResetCode::kDuplicateConfirmed:
      return result.confirmed;
    case PlaybackReferenceResetCode::kInvalidConfirmation:
    case PlaybackReferenceResetCode::kNotAwaiting:
    case PlaybackReferenceResetCode::kRequestMismatch:
    case PlaybackReferenceResetCode::kGenerationMismatch:
    case PlaybackReferenceResetCode::kSinkFault:
      return !result.confirmed;
    case PlaybackReferenceResetCode::kUnset:
      return false;
  }
  return false;
}

bool CommittedCancelIdentityMatches(
    const PlaybackCancelResult& result, const std::uint64_t request_id,
    const PlaybackGeneration& generation) noexcept {
  return result.request_id == request_id &&
         GenerationsEqual(result.generation, generation);
}

bool PlaybackCancelResultIsZero(const PlaybackCancelResult& result) noexcept {
  return result.code == PlaybackCancelCode::kUnset && result.request_id == 0U &&
         GenerationIsZero(result.generation) &&
         result.observed_cancel_epoch == 0U &&
         result.retired_pcm_incarnation == 0U &&
         result.prepared_pcm_incarnation == 0U &&
         result.accepted_generation_sample_frames == 0U &&
         result.last_accepted_chunk_sequence == 0U &&
         result.accepted_after_cancel_fence_sample_frames == 0U &&
         result.discarded_pending_sample_frames == 0U &&
         result.native_error == 0 && !result.drop_succeeded &&
         !result.prepare_succeeded && !result.reference_reset_required &&
         !result.acknowledged;
}

bool PlaybackCancelResultsEqual(const PlaybackCancelResult& lhs,
                                const PlaybackCancelResult& rhs) noexcept {
  const bool codes_equivalent =
      lhs.code == rhs.code ||
      ((lhs.code == PlaybackCancelCode::kAcknowledged ||
        lhs.code == PlaybackCancelCode::kDuplicateAcknowledged) &&
       (rhs.code == PlaybackCancelCode::kAcknowledged ||
        rhs.code == PlaybackCancelCode::kDuplicateAcknowledged));
  return codes_equivalent && lhs.request_id == rhs.request_id &&
         GenerationsEqual(lhs.generation, rhs.generation) &&
         lhs.observed_cancel_epoch == rhs.observed_cancel_epoch &&
         lhs.retired_pcm_incarnation == rhs.retired_pcm_incarnation &&
         lhs.prepared_pcm_incarnation == rhs.prepared_pcm_incarnation &&
         lhs.accepted_generation_sample_frames ==
             rhs.accepted_generation_sample_frames &&
         lhs.last_accepted_chunk_sequence == rhs.last_accepted_chunk_sequence &&
         lhs.accepted_after_cancel_fence_sample_frames ==
             rhs.accepted_after_cancel_fence_sample_frames &&
         lhs.discarded_pending_sample_frames ==
             rhs.discarded_pending_sample_frames &&
         lhs.native_error == rhs.native_error &&
         lhs.drop_succeeded == rhs.drop_succeeded &&
         lhs.prepare_succeeded == rhs.prepare_succeeded &&
         lhs.reference_reset_required == rhs.reference_reset_required &&
         lhs.acknowledged == rhs.acknowledged;
}

bool CommitterCancellationBarrierSucceeded(
    const PlaybackCancelResult& result) noexcept {
  return result.acknowledged && result.reference_reset_required &&
         result.drop_succeeded && result.prepare_succeeded &&
         result.retired_pcm_incarnation != 0U &&
         result.retired_pcm_incarnation <
             std::numeric_limits<std::uint64_t>::max() &&
         result.prepared_pcm_incarnation ==
             result.retired_pcm_incarnation + 1U &&
         result.native_error == 0;
}

bool ProducerStopAcksEqual(const TtsProducerStopAck& lhs,
                           const TtsProducerStopAck& rhs) noexcept {
  return lhs.code == rhs.code && lhs.request_id == rhs.request_id &&
         GenerationsEqual(lhs.generation, rhs.generation) &&
         lhs.observed_cancel_epoch == rhs.observed_cancel_epoch &&
         lhs.producer_stopped == rhs.producer_stopped &&
         lhs.producer_write_lease_released == rhs.producer_write_lease_released;
}

bool LocalCancelCompletionsEqual(
    const PlaybackLocalCancelCompletion& lhs,
    const PlaybackLocalCancelCompletion& rhs) noexcept {
  return lhs.code == rhs.code && lhs.request_id == rhs.request_id &&
         GenerationsEqual(lhs.generation, rhs.generation) &&
         lhs.observed_fence_epoch == rhs.observed_fence_epoch &&
         PlaybackCancelResultsEqual(lhs.committer_result,
                                    rhs.committer_result) &&
         lhs.initial_ingress_drain.discarded_frames ==
             rhs.initial_ingress_drain.discarded_frames &&
         lhs.initial_ingress_drain.discarded_source_samples ==
             rhs.initial_ingress_drain.discarded_source_samples &&
         lhs.initial_ingress_drain.reached_iteration_bound ==
             rhs.initial_ingress_drain.reached_iteration_bound &&
         lhs.worker_holds_no_ingress_lease ==
             rhs.worker_holds_no_ingress_lease &&
         lhs.producer_lease_may_publish_late ==
             rhs.producer_lease_may_publish_late &&
         lhs.reference_reset_required == rhs.reference_reset_required &&
         lhs.renderer_generation_never_armed ==
             rhs.renderer_generation_never_armed &&
         lhs.renderer_disarmed_after_cancel_fence ==
             rhs.renderer_disarmed_after_cancel_fence &&
         lhs.committer_generation_never_armed ==
             rhs.committer_generation_never_armed &&
         lhs.committer_prepared_idle == rhs.committer_prepared_idle &&
         lhs.no_accepted_pcm_for_generation ==
             rhs.no_accepted_pcm_for_generation;
}

bool IngressQuiescedAcksEqual(const PlaybackIngressQuiescedAck& lhs,
                              const PlaybackIngressQuiescedAck& rhs) noexcept {
  return lhs.code == rhs.code && lhs.request_id == rhs.request_id &&
         GenerationsEqual(lhs.generation, rhs.generation) &&
         lhs.retired_pcm_incarnation == rhs.retired_pcm_incarnation &&
         lhs.discarded_frames == rhs.discarded_frames &&
         lhs.discarded_source_samples == rhs.discarded_source_samples &&
         lhs.producer_stop_precondition == rhs.producer_stop_precondition &&
         lhs.empty_observed_after_producer_stop ==
             rhs.empty_observed_after_producer_stop &&
         lhs.reached_iteration_bound == rhs.reached_iteration_bound;
}

bool DspReferenceResetAcksEqual(const DspReferenceResetAck& lhs,
                                const DspReferenceResetAck& rhs) noexcept {
  return lhs.code == rhs.code && lhs.request_id == rhs.request_id &&
         GenerationsEqual(lhs.generation, rhs.generation) &&
         lhs.retired_pcm_incarnation == rhs.retired_pcm_incarnation &&
         lhs.accepted_consumer_lease_released ==
             rhs.accepted_consumer_lease_released &&
         lhs.retired_incarnation_discarded ==
             rhs.retired_incarnation_discarded &&
         lhs.accepted_queue_empty_after_local_cancel ==
             rhs.accepted_queue_empty_after_local_cancel &&
         lhs.dsp_history_reset == rhs.dsp_history_reset;
}

bool ReferenceResetResultsEquivalent(
    const PlaybackReferenceResetResult& lhs,
    const PlaybackReferenceResetResult& rhs) noexcept {
  return lhs.request_id == rhs.request_id &&
         GenerationsEqual(lhs.generation, rhs.generation) && lhs.confirmed &&
         rhs.confirmed;
}

}  // namespace

bool PlaybackLifecycleCommand::valid() const noexcept {
  if (request_id == 0U) {
    return false;
  }
  switch (type) {
    case PlaybackLifecycleCommandType::kArm:
      return generation.valid() && retired_pcm_incarnation == 0U && !value;
    case PlaybackLifecycleCommandType::kSetDucked:
      return generation.valid() && retired_pcm_incarnation == 0U;
    case PlaybackLifecycleCommandType::kQuiesceIngress:
      return generation.valid() && value;
    case PlaybackLifecycleCommandType::kConfirmReferenceReset:
      return generation.valid() && retired_pcm_incarnation != 0U && !value;
    case PlaybackLifecycleCommandType::kShutdown:
      return GenerationIsZero(generation) && retired_pcm_incarnation == 0U &&
             !value;
    case PlaybackLifecycleCommandType::kUnset:
      return false;
  }
  return false;
}

bool PlaybackCancelCommand::valid() const noexcept {
  return request_id != 0U && generation.valid() &&
         cancel_epoch > generation.epoch;
}

bool PlaybackArmResult::acknowledged() const noexcept {
  return code == PlaybackArmResultCode::kAcknowledged;
}

bool PlaybackArmResult::valid() const noexcept {
  if (request_id == 0U || !generation.valid()) {
    return false;
  }
  switch (code) {
    case PlaybackArmResultCode::kAcknowledged:
      return observed_fence_epoch == generation.epoch;
    case PlaybackArmResultCode::kFenceMismatch:
    case PlaybackArmResultCode::kCancelledBeforeAcknowledgement:
      return observed_fence_epoch != 0U &&
             observed_fence_epoch != generation.epoch;
    case PlaybackArmResultCode::kInvalidCommand:
    case PlaybackArmResultCode::kRendererRejected:
    case PlaybackArmResultCode::kCommitterRejected:
    case PlaybackArmResultCode::kFaulted:
      return observed_fence_epoch != 0U;
    case PlaybackArmResultCode::kUnset:
      return false;
  }
  return false;
}

bool PlaybackDuckResult::acknowledged() const noexcept {
  return code == PlaybackDuckResultCode::kAcknowledged;
}

bool PlaybackDuckResult::valid() const noexcept {
  if (request_id == 0U || !generation.valid()) {
    return false;
  }
  switch (code) {
    case PlaybackDuckResultCode::kAcknowledged:
    case PlaybackDuckResultCode::kInvalidCommand:
    case PlaybackDuckResultCode::kNotArmed:
    case PlaybackDuckResultCode::kGenerationMismatch:
    case PlaybackDuckResultCode::kRendererRejected:
      return true;
    case PlaybackDuckResultCode::kUnset:
      return false;
  }
  return false;
}

bool PlaybackIngressQuiescedAck::acknowledged() const noexcept {
  return code == PlaybackIngressQuiesceCode::kAcknowledged &&
         producer_stop_precondition && empty_observed_after_producer_stop &&
         !reached_iteration_bound;
}

bool PlaybackIngressQuiescedAck::valid() const noexcept {
  if (request_id == 0U || !generation.valid()) {
    return false;
  }
  switch (code) {
    case PlaybackIngressQuiesceCode::kAcknowledged:
      return acknowledged();
    case PlaybackIngressQuiesceCode::kInProgress:
      return producer_stop_precondition &&
             !empty_observed_after_producer_stop && reached_iteration_bound;
    case PlaybackIngressQuiesceCode::kInvalidCommand:
    case PlaybackIngressQuiesceCode::kProducerNotStopped:
    case PlaybackIngressQuiesceCode::kGenerationMismatch:
    case PlaybackIngressQuiesceCode::kIncarnationMismatch:
    case PlaybackIngressQuiesceCode::kFaulted:
      return !empty_observed_after_producer_stop;
    case PlaybackIngressQuiesceCode::kUnset:
      return false;
  }
  return false;
}

bool PlaybackShutdownResult::acknowledged() const noexcept {
  return code == PlaybackShutdownResultCode::kAcknowledged;
}

bool PlaybackShutdownResult::valid() const noexcept {
  if (request_id == 0U) {
    return false;
  }
  switch (code) {
    case PlaybackShutdownResultCode::kAcknowledged:
    case PlaybackShutdownResultCode::kInvalidCommand:
    case PlaybackShutdownResultCode::kActiveGeneration:
    case PlaybackShutdownResultCode::kFaulted:
      return true;
    case PlaybackShutdownResultCode::kUnset:
      return false;
  }
  return false;
}

bool PlaybackLifecycleResult::valid() const noexcept {
  switch (type) {
    case PlaybackLifecycleResultType::kArm:
      return arm.valid();
    case PlaybackLifecycleResultType::kDuck:
      return duck.valid();
    case PlaybackLifecycleResultType::kIngressQuiesced:
      return ingress_quiesced.valid();
    case PlaybackLifecycleResultType::kReferenceReset:
      return ReferenceResetResultValid(reference_reset);
    case PlaybackLifecycleResultType::kShutdown:
      return shutdown.valid();
    case PlaybackLifecycleResultType::kUnset:
      return false;
  }
  return false;
}

bool TtsProducerStartPermit::valid() const noexcept {
  return request_id != 0U && generation.valid();
}

bool TtsProducerStopRequest::valid() const noexcept {
  return request_id != 0U && generation.valid() &&
         cancel_epoch > generation.epoch;
}

bool TtsProducerStartAck::acknowledged() const noexcept {
  return code == TtsProducerStartCode::kAcknowledged && publish_enabled &&
         owns_no_preexisting_write_lease;
}

bool TtsProducerStartAck::valid() const noexcept {
  if (request_id == 0U || !generation.valid()) {
    return false;
  }
  switch (code) {
    case TtsProducerStartCode::kAcknowledged:
      return acknowledged();
    case TtsProducerStartCode::kGenerationMismatch:
    case TtsProducerStartCode::kAlreadyStarted:
      return !publish_enabled && !owns_no_preexisting_write_lease;
    case TtsProducerStartCode::kFailed:
      return !publish_enabled;
    case TtsProducerStartCode::kUnset:
      return false;
  }
  return false;
}

bool PlaybackLocalCancelCompletion::acknowledged() const noexcept {
  if (code != PlaybackLocalCancelCode::kAcknowledged ||
      !reference_reset_required || !worker_holds_no_ingress_lease ||
      !producer_lease_may_publish_late ||
      !CommitterCancellationBarrierSucceeded(committer_result) ||
      !renderer_disarmed_after_cancel_fence || committer_prepared_idle ||
      renderer_generation_never_armed || committer_generation_never_armed ||
      no_accepted_pcm_for_generation) {
    return false;
  }
  return committer_result.code == PlaybackCancelCode::kAcknowledged ||
         committer_result.code == PlaybackCancelCode::kDuplicateAcknowledged;
}

bool PlaybackLocalCancelCompletion::already_quiescent() const noexcept {
  return code == PlaybackLocalCancelCode::kAlreadyQuiescent &&
         !reference_reset_required && worker_holds_no_ingress_lease &&
         producer_lease_may_publish_late && renderer_generation_never_armed &&
         !renderer_disarmed_after_cancel_fence &&
         committer_generation_never_armed && committer_prepared_idle &&
         no_accepted_pcm_for_generation &&
         PlaybackCancelResultIsZero(committer_result);
}

bool PlaybackLocalCancelCompletion::renderer_only_quiesced() const noexcept {
  return code == PlaybackLocalCancelCode::kRendererOnlyQuiesced &&
         !reference_reset_required && worker_holds_no_ingress_lease &&
         producer_lease_may_publish_late && !renderer_generation_never_armed &&
         renderer_disarmed_after_cancel_fence &&
         committer_generation_never_armed && committer_prepared_idle &&
         no_accepted_pcm_for_generation &&
         PlaybackCancelResultIsZero(committer_result);
}

bool PlaybackLocalCancelCompletion::terminal_restart_required() const noexcept {
  return code == PlaybackLocalCancelCode::kRestartRequired &&
         committer_result.code == PlaybackCancelCode::kRestartRequired &&
         reference_reset_required && producer_lease_may_publish_late &&
         renderer_disarmed_after_cancel_fence && !committer_prepared_idle &&
         !renderer_generation_never_armed &&
         !committer_generation_never_armed && !no_accepted_pcm_for_generation &&
         CommitterCancellationBarrierSucceeded(committer_result);
}

bool PlaybackLocalCancelCompletion::valid() const noexcept {
  if (request_id == 0U || !generation.valid() ||
      !worker_holds_no_ingress_lease) {
    return false;
  }
  switch (code) {
    case PlaybackLocalCancelCode::kAcknowledged:
      return observed_fence_epoch > generation.epoch && acknowledged() &&
             CommittedCancelIdentityMatches(committer_result, request_id,
                                            generation) &&
             committer_result.observed_cancel_epoch == observed_fence_epoch;
    case PlaybackLocalCancelCode::kAlreadyQuiescent:
      return observed_fence_epoch > generation.epoch && already_quiescent();
    case PlaybackLocalCancelCode::kRendererOnlyQuiesced:
      return observed_fence_epoch > generation.epoch &&
             renderer_only_quiesced();
    case PlaybackLocalCancelCode::kRestartRequired:
      return observed_fence_epoch > generation.epoch &&
             terminal_restart_required() &&
             CommittedCancelIdentityMatches(committer_result, request_id,
                                            generation) &&
             committer_result.observed_cancel_epoch == observed_fence_epoch;
    case PlaybackLocalCancelCode::kFenceNotAdvanced:
      return observed_fence_epoch <= generation.epoch &&
             committer_result.code == PlaybackCancelCode::kFenceNotAdvanced &&
             !committer_result.acknowledged &&
             CommittedCancelIdentityMatches(committer_result, request_id,
                                            generation) &&
             committer_result.observed_cancel_epoch == observed_fence_epoch;
    case PlaybackLocalCancelCode::kInvalidCommand:
    case PlaybackLocalCancelCode::kGenerationMismatch:
    case PlaybackLocalCancelCode::kRejected:
    case PlaybackLocalCancelCode::kFaulted:
      return !committer_result.acknowledged;
    case PlaybackLocalCancelCode::kUnset:
      return false;
  }
  return false;
}

bool TtsProducerStopAck::acknowledged() const noexcept {
  return code == TtsProducerStopCode::kAcknowledged &&
         observed_cancel_epoch > generation.epoch && producer_stopped &&
         producer_write_lease_released;
}

bool TtsProducerStopAck::valid() const noexcept {
  if (request_id == 0U || !generation.valid()) {
    return false;
  }
  switch (code) {
    case TtsProducerStopCode::kAcknowledged:
      return acknowledged();
    case TtsProducerStopCode::kInvalidRequest:
      return observed_cancel_epoch <= generation.epoch && !producer_stopped &&
             !producer_write_lease_released;
    case TtsProducerStopCode::kGenerationMismatch:
      return observed_cancel_epoch > generation.epoch && !producer_stopped &&
             !producer_write_lease_released;
    case TtsProducerStopCode::kFailed:
      return observed_cancel_epoch > generation.epoch &&
             !(producer_stopped && producer_write_lease_released) &&
             (!producer_write_lease_released || producer_stopped);
    case TtsProducerStopCode::kUnset:
      return false;
  }
  return false;
}

bool DspReferenceResetRequest::valid() const noexcept {
  return request_id != 0U && generation.valid() &&
         retired_pcm_incarnation != 0U;
}

bool DspReferenceResetAck::acknowledged() const noexcept {
  return code == DspReferenceResetCode::kAcknowledged &&
         retired_pcm_incarnation != 0U && accepted_consumer_lease_released &&
         retired_incarnation_discarded &&
         accepted_queue_empty_after_local_cancel && dsp_history_reset;
}

bool DspReferenceResetAck::valid() const noexcept {
  if (request_id == 0U || !generation.valid()) {
    return false;
  }
  switch (code) {
    case DspReferenceResetCode::kAcknowledged:
      return acknowledged();
    case DspReferenceResetCode::kInvalidRequest:
      return retired_pcm_incarnation == 0U &&
             !accepted_consumer_lease_released &&
             !retired_incarnation_discarded &&
             !accepted_queue_empty_after_local_cancel && !dsp_history_reset;
    case DspReferenceResetCode::kGenerationMismatch:
    case DspReferenceResetCode::kIncarnationMismatch:
    case DspReferenceResetCode::kUnavailable:
      return retired_pcm_incarnation != 0U &&
             !accepted_consumer_lease_released &&
             !retired_incarnation_discarded &&
             !accepted_queue_empty_after_local_cancel && !dsp_history_reset;
    case DspReferenceResetCode::kBackendFailed:
      return retired_pcm_incarnation != 0U && !dsp_history_reset &&
             (!retired_incarnation_discarded ||
              accepted_consumer_lease_released) &&
             (!accepted_queue_empty_after_local_cancel ||
              retired_incarnation_discarded);
    case DspReferenceResetCode::kUnset:
      return false;
  }
  return false;
}

bool PlaybackEndOfStreamAcceptedEvent::valid() const noexcept {
  return event_sequence != 0U && generation.valid() && pcm_incarnation != 0U &&
         accepted_generation_sample_frames != 0U &&
         last_accepted_chunk_sequence != 0U;
}

bool PlaybackCriticalStreamEvent::valid() const noexcept {
  if (event_sequence == 0U) {
    return false;
  }
  const bool counters_are_zero = accepted_generation_sample_frames == 0U &&
                                 last_accepted_chunk_sequence == 0U;
  const bool counters_are_scoped = accepted_generation_sample_frames != 0U &&
                                   last_accepted_chunk_sequence != 0U;
  switch (code) {
    case PlaybackCriticalStreamEventCode::kCancellationRequired:
      return generation.valid() && pcm_incarnation != 0U &&
             (counters_are_zero || counters_are_scoped) && native_error <= 0;
    case PlaybackCriticalStreamEventCode::kFaulted:
      if (GenerationIsZero(generation)) {
        return pcm_incarnation == 0U && counters_are_zero && native_error < 0;
      }
      return generation.valid() && pcm_incarnation != 0U &&
             (counters_are_zero || counters_are_scoped) && native_error < 0;
    case PlaybackCriticalStreamEventCode::kUnset:
      return false;
  }
  return false;
}

PlaybackCancellationJoinResult PlaybackCancellationJoin::Begin(
    const PlaybackCancelCommand& command) noexcept {
  if (!command.valid()) {
    return Snapshot(PlaybackCancellationJoinCode::kInvalidInput);
  }
  if (state_ == PlaybackCancellationJoinState::kAwaitingBarriers ||
      state_ == PlaybackCancellationJoinState::kReadyForPlaybackConfirmation) {
    if (Matches(command.request_id, command.generation)) {
      return Snapshot(
          command.cancel_epoch == cancel_epoch_
              ? PlaybackCancellationJoinCode::kDuplicate
              : PlaybackCancellationJoinCode::kConflictingDuplicate);
    }
    return Snapshot(PlaybackCancellationJoinCode::kOutOfOrder);
  }
  if (state_ == PlaybackCancellationJoinState::kFaulted) {
    return Snapshot(PlaybackCancellationJoinCode::kBarrierFailed);
  }
  if (state_ == PlaybackCancellationJoinState::kCompleted) {
    if (Matches(command.request_id, command.generation)) {
      return Snapshot(
          command.cancel_epoch == cancel_epoch_
              ? PlaybackCancellationJoinCode::kDuplicate
              : PlaybackCancellationJoinCode::kConflictingDuplicate);
    }
    if (command.request_id <= request_id_) {
      return Snapshot(PlaybackCancellationJoinCode::kRequestNotIncreasing);
    }
    // The previous cancellation already advanced the actor-owned fence to
    // cancel_epoch_. A later generation below that floor can never have been
    // legally Arm'ed, even when its epoch is numerically newer than the old
    // generation. The next cancellation fence must advance again as well.
    if (command.generation.epoch < cancel_epoch_ ||
        command.cancel_epoch <= cancel_epoch_) {
      return Snapshot(PlaybackCancellationJoinCode::kEpochNotIncreasing);
    }
  }

  state_ = PlaybackCancellationJoinState::kAwaitingBarriers;
  request_id_ = command.request_id;
  generation_ = command.generation;
  cancel_epoch_ = command.cancel_epoch;
  retired_pcm_incarnation_ = 0U;
  producer_stopped_ = false;
  playback_local_cancelled_ = false;
  ingress_quiesced_ = false;
  dsp_reference_reset_ = false;
  playback_reference_reset_confirmed_ = false;
  reference_reset_required_ = false;
  local_no_reference_path_ = false;
  producer_stop_ack_ = {};
  local_cancel_completion_ = {};
  ingress_quiesced_ack_ = {};
  dsp_reference_reset_ack_ = {};
  playback_reference_reset_result_ = {};
  return Snapshot(PlaybackCancellationJoinCode::kStarted);
}

PlaybackCancellationJoinResult PlaybackCancellationJoin::ObserveProducerStop(
    const TtsProducerStopAck& ack) noexcept {
  if ((state_ == PlaybackCancellationJoinState::kReadyForPlaybackConfirmation ||
       state_ == PlaybackCancellationJoinState::kCompleted) &&
      Matches(ack.request_id, ack.generation)) {
    if (producer_stopped_ && ack.valid() && ack.acknowledged() &&
        ProducerStopAcksEqual(producer_stop_ack_, ack)) {
      return Snapshot(PlaybackCancellationJoinCode::kDuplicate);
    }
    state_ = PlaybackCancellationJoinState::kFaulted;
    return Snapshot(PlaybackCancellationJoinCode::kConflictingDuplicate);
  }
  if (state_ != PlaybackCancellationJoinState::kAwaitingBarriers) {
    return Snapshot(PlaybackCancellationJoinCode::kNotActive);
  }
  if (!Matches(ack.request_id, ack.generation)) {
    return Snapshot(ack.request_id != request_id_
                        ? PlaybackCancellationJoinCode::kRequestMismatch
                        : PlaybackCancellationJoinCode::kGenerationMismatch);
  }
  if (!ack.valid()) {
    return Snapshot(PlaybackCancellationJoinCode::kInvalidInput);
  }
  if (ack.observed_cancel_epoch != cancel_epoch_) {
    return Snapshot(PlaybackCancellationJoinCode::kFenceEpochMismatch);
  }
  if (!ack.acknowledged()) {
    state_ = PlaybackCancellationJoinState::kFaulted;
    return Snapshot(PlaybackCancellationJoinCode::kBarrierFailed);
  }
  if (producer_stopped_) {
    if (!ProducerStopAcksEqual(producer_stop_ack_, ack)) {
      state_ = PlaybackCancellationJoinState::kFaulted;
      return Snapshot(PlaybackCancellationJoinCode::kConflictingDuplicate);
    }
    return Snapshot(PlaybackCancellationJoinCode::kDuplicate);
  }
  producer_stopped_ = true;
  producer_stop_ack_ = ack;
  return EvaluateProgress();
}

PlaybackCancellationJoinResult PlaybackCancellationJoin::ObserveLocalCancel(
    const PlaybackLocalCancelCompletion& completion) noexcept {
  if ((state_ == PlaybackCancellationJoinState::kReadyForPlaybackConfirmation ||
       state_ == PlaybackCancellationJoinState::kCompleted) &&
      Matches(completion.request_id, completion.generation)) {
    if (playback_local_cancelled_ && completion.valid() &&
        LocalCancelCompletionsEqual(local_cancel_completion_, completion)) {
      return Snapshot(PlaybackCancellationJoinCode::kDuplicate);
    }
    state_ = PlaybackCancellationJoinState::kFaulted;
    return Snapshot(PlaybackCancellationJoinCode::kConflictingDuplicate);
  }
  if (state_ != PlaybackCancellationJoinState::kAwaitingBarriers) {
    return Snapshot(PlaybackCancellationJoinCode::kNotActive);
  }
  if (!Matches(completion.request_id, completion.generation)) {
    return Snapshot(completion.request_id != request_id_
                        ? PlaybackCancellationJoinCode::kRequestMismatch
                        : PlaybackCancellationJoinCode::kGenerationMismatch);
  }
  if (!completion.valid()) {
    return Snapshot(PlaybackCancellationJoinCode::kInvalidInput);
  }
  if (completion.terminal_restart_required()) {
    state_ = PlaybackCancellationJoinState::kFaulted;
    return Snapshot(PlaybackCancellationJoinCode::kTerminalRestartRequired);
  }
  if (!completion.acknowledged() && !completion.already_quiescent() &&
      !completion.renderer_only_quiesced()) {
    state_ = PlaybackCancellationJoinState::kFaulted;
    return Snapshot(PlaybackCancellationJoinCode::kBarrierFailed);
  }
  if (completion.observed_fence_epoch != cancel_epoch_) {
    return Snapshot(PlaybackCancellationJoinCode::kFenceEpochMismatch);
  }
  if (playback_local_cancelled_) {
    if (!LocalCancelCompletionsEqual(local_cancel_completion_, completion)) {
      state_ = PlaybackCancellationJoinState::kFaulted;
      return Snapshot(PlaybackCancellationJoinCode::kConflictingDuplicate);
    }
    return Snapshot(PlaybackCancellationJoinCode::kDuplicate);
  }

  playback_local_cancelled_ = true;
  local_cancel_completion_ = completion;
  reference_reset_required_ = completion.reference_reset_required;
  local_no_reference_path_ =
      completion.already_quiescent() || completion.renderer_only_quiesced();
  retired_pcm_incarnation_ =
      completion.acknowledged()
          ? completion.committer_result.retired_pcm_incarnation
          : 0U;
  return EvaluateProgress();
}

PlaybackCancellationJoinResult PlaybackCancellationJoin::ObserveIngressQuiesced(
    const PlaybackIngressQuiescedAck& ack) noexcept {
  if ((state_ == PlaybackCancellationJoinState::kReadyForPlaybackConfirmation ||
       state_ == PlaybackCancellationJoinState::kCompleted) &&
      Matches(ack.request_id, ack.generation)) {
    if (ingress_quiesced_ && ack.valid() && ack.acknowledged() &&
        IngressQuiescedAcksEqual(ingress_quiesced_ack_, ack)) {
      return Snapshot(PlaybackCancellationJoinCode::kDuplicate);
    }
    state_ = PlaybackCancellationJoinState::kFaulted;
    return Snapshot(PlaybackCancellationJoinCode::kConflictingDuplicate);
  }
  if (state_ != PlaybackCancellationJoinState::kAwaitingBarriers) {
    return Snapshot(PlaybackCancellationJoinCode::kNotActive);
  }
  if (!Matches(ack.request_id, ack.generation)) {
    return Snapshot(ack.request_id != request_id_
                        ? PlaybackCancellationJoinCode::kRequestMismatch
                        : PlaybackCancellationJoinCode::kGenerationMismatch);
  }
  if (!producer_stopped_ || !playback_local_cancelled_) {
    return Snapshot(PlaybackCancellationJoinCode::kOutOfOrder);
  }
  if (!ack.valid()) {
    return Snapshot(PlaybackCancellationJoinCode::kInvalidInput);
  }
  if (ack.retired_pcm_incarnation != retired_pcm_incarnation_) {
    return Snapshot(PlaybackCancellationJoinCode::kIncarnationMismatch);
  }
  if (ingress_quiesced_) {
    if (ack.acknowledged() &&
        IngressQuiescedAcksEqual(ingress_quiesced_ack_, ack)) {
      return Snapshot(PlaybackCancellationJoinCode::kDuplicate);
    }
    state_ = PlaybackCancellationJoinState::kFaulted;
    return Snapshot(PlaybackCancellationJoinCode::kConflictingDuplicate);
  }
  if (!ack.acknowledged()) {
    if (ack.code == PlaybackIngressQuiesceCode::kInProgress) {
      return Snapshot(PlaybackCancellationJoinCode::kProgress);
    }
    state_ = PlaybackCancellationJoinState::kFaulted;
    return Snapshot(PlaybackCancellationJoinCode::kBarrierFailed);
  }
  ingress_quiesced_ = true;
  ingress_quiesced_ack_ = ack;
  return EvaluateProgress();
}

PlaybackCancellationJoinResult
PlaybackCancellationJoin::ObserveDspReferenceReset(
    const DspReferenceResetAck& ack) noexcept {
  if ((state_ == PlaybackCancellationJoinState::kReadyForPlaybackConfirmation ||
       state_ == PlaybackCancellationJoinState::kCompleted) &&
      Matches(ack.request_id, ack.generation)) {
    if (dsp_reference_reset_ && ack.valid() && ack.acknowledged() &&
        DspReferenceResetAcksEqual(dsp_reference_reset_ack_, ack)) {
      return Snapshot(PlaybackCancellationJoinCode::kDuplicate);
    }
    state_ = PlaybackCancellationJoinState::kFaulted;
    return Snapshot(PlaybackCancellationJoinCode::kConflictingDuplicate);
  }
  if (state_ != PlaybackCancellationJoinState::kAwaitingBarriers) {
    return Snapshot(PlaybackCancellationJoinCode::kNotActive);
  }
  if (!Matches(ack.request_id, ack.generation)) {
    return Snapshot(ack.request_id != request_id_
                        ? PlaybackCancellationJoinCode::kRequestMismatch
                        : PlaybackCancellationJoinCode::kGenerationMismatch);
  }
  if (!playback_local_cancelled_ || !reference_reset_required_) {
    return Snapshot(PlaybackCancellationJoinCode::kOutOfOrder);
  }
  if (!ack.valid()) {
    return Snapshot(PlaybackCancellationJoinCode::kInvalidInput);
  }
  if (ack.retired_pcm_incarnation != retired_pcm_incarnation_) {
    return Snapshot(PlaybackCancellationJoinCode::kIncarnationMismatch);
  }
  if (!ack.acknowledged()) {
    state_ = PlaybackCancellationJoinState::kFaulted;
    return Snapshot(PlaybackCancellationJoinCode::kBarrierFailed);
  }
  if (dsp_reference_reset_) {
    if (!DspReferenceResetAcksEqual(dsp_reference_reset_ack_, ack)) {
      state_ = PlaybackCancellationJoinState::kFaulted;
      return Snapshot(PlaybackCancellationJoinCode::kConflictingDuplicate);
    }
    return Snapshot(PlaybackCancellationJoinCode::kDuplicate);
  }
  dsp_reference_reset_ = true;
  dsp_reference_reset_ack_ = ack;
  return EvaluateProgress();
}

PlaybackCancellationJoinResult
PlaybackCancellationJoin::ObservePlaybackReferenceReset(
    const PlaybackReferenceResetResult& result) noexcept {
  if (state_ != PlaybackCancellationJoinState::kReadyForPlaybackConfirmation) {
    if (state_ == PlaybackCancellationJoinState::kCompleted &&
        Matches(result.request_id, result.generation)) {
      if (!ReferenceResetResultValid(result) || !result.confirmed ||
          !ReferenceResetResultsEquivalent(playback_reference_reset_result_,
                                           result)) {
        state_ = PlaybackCancellationJoinState::kFaulted;
        return Snapshot(PlaybackCancellationJoinCode::kConflictingDuplicate);
      }
      return Snapshot(PlaybackCancellationJoinCode::kDuplicate);
    }
    return Snapshot(PlaybackCancellationJoinCode::kOutOfOrder);
  }
  if (!Matches(result.request_id, result.generation)) {
    return Snapshot(result.request_id != request_id_
                        ? PlaybackCancellationJoinCode::kRequestMismatch
                        : PlaybackCancellationJoinCode::kGenerationMismatch);
  }
  if (!ReferenceResetResultValid(result)) {
    return Snapshot(PlaybackCancellationJoinCode::kInvalidInput);
  }
  if (!result.confirmed) {
    state_ = PlaybackCancellationJoinState::kFaulted;
    return Snapshot(PlaybackCancellationJoinCode::kBarrierFailed);
  }

  playback_reference_reset_confirmed_ = true;
  playback_reference_reset_result_ = result;
  state_ = PlaybackCancellationJoinState::kCompleted;
  return Snapshot(PlaybackCancellationJoinCode::kCompleted);
}

PlaybackCancellationJoinResult PlaybackCancellationJoin::Snapshot(
    const PlaybackCancellationJoinCode code) const noexcept {
  return {
      code,
      state_,
      request_id_,
      generation_,
      cancel_epoch_,
      retired_pcm_incarnation_,
      producer_stopped_,
      playback_local_cancelled_,
      ingress_quiesced_,
      dsp_reference_reset_,
      playback_reference_reset_confirmed_,
      state_ == PlaybackCancellationJoinState::kReadyForPlaybackConfirmation,
      state_ == PlaybackCancellationJoinState::kCompleted};
}

PlaybackCancellationJoinResult
PlaybackCancellationJoin::EvaluateProgress() noexcept {
  if (!producer_stopped_ || !playback_local_cancelled_ || !ingress_quiesced_) {
    return Snapshot(PlaybackCancellationJoinCode::kProgress);
  }

  if (local_no_reference_path_ && !reference_reset_required_) {
    state_ = PlaybackCancellationJoinState::kCompleted;
    return Snapshot(PlaybackCancellationJoinCode::kCompleted);
  }

  if (reference_reset_required_ && dsp_reference_reset_) {
    state_ = PlaybackCancellationJoinState::kReadyForPlaybackConfirmation;
    return Snapshot(
        PlaybackCancellationJoinCode::kReadyForPlaybackConfirmation);
  }
  return Snapshot(PlaybackCancellationJoinCode::kProgress);
}

bool PlaybackCancellationJoin::Matches(
    const std::uint64_t request_id,
    const PlaybackGeneration& generation) const noexcept {
  return request_id == request_id_ && GenerationsEqual(generation, generation_);
}

}  // namespace boompi::audio
