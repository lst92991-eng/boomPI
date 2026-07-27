#include "boompi/audio/playback_committer.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>

namespace boompi::audio {
namespace {

static_assert(std::is_trivially_copyable<PlaybackSubmitResult>::value,
              "playback submit result must remain trivially copyable");
static_assert(std::is_standard_layout<PlaybackSubmitResult>::value,
              "playback submit result must remain standard-layout");
static_assert(std::is_trivially_copyable<PlaybackCommitResult>::value,
              "playback commit result must remain trivially copyable");
static_assert(std::is_standard_layout<PlaybackCommitResult>::value,
              "playback commit result must remain standard-layout");
static_assert(std::is_trivially_copyable<PlaybackCancelResult>::value,
              "playback cancel result must remain trivially copyable");
static_assert(std::is_standard_layout<PlaybackCancelResult>::value,
              "playback cancel result must remain standard-layout");
static_assert(
    std::is_trivially_copyable<PlaybackReferenceResetResult>::value,
    "playback reference reset result must remain trivially copyable");
static_assert(std::is_standard_layout<PlaybackReferenceResetResult>::value,
              "playback reference reset result must remain standard-layout");

bool GenerationsEqual(const PlaybackGeneration& lhs,
                      const PlaybackGeneration& rhs) noexcept {
  return lhs.epoch == rhs.epoch && lhs.turn_id == rhs.turn_id &&
         lhs.stream_id == rhs.stream_id;
}

PlaybackGeneration GenerationOf(const PlaybackPcmFrame48k& frame) noexcept {
  return {frame.metadata.epoch, frame.metadata.turn_id,
          frame.metadata.stream_id};
}

bool MetadataEqual(const AudioFrameMetadata& lhs,
                   const AudioFrameMetadata& rhs) noexcept {
  return lhs.monotonic_timestamp_us == rhs.monotonic_timestamp_us &&
         lhs.sequence == rhs.sequence && lhs.stream_id == rhs.stream_id &&
         lhs.turn_id == rhs.turn_id && lhs.epoch == rhs.epoch &&
         lhs.discontinuity == rhs.discontinuity;
}

bool IsKnownReferencePolicy(const PlaybackReferencePolicy policy) noexcept {
  return policy == PlaybackReferencePolicy::kRequiredSoftwareReference ||
         policy == PlaybackReferencePolicy::kHardwareCaptureReference;
}

bool IsValidTiming(const PcmPlaybackWriteResult& result) noexcept {
  const bool timing_source_valid =
      result.timing_source == PlaybackTimingSource::kSoftwareEstimate ||
      result.timing_source == PlaybackTimingSource::kAlsaStatus;
  return result.write_completed_timestamp_us != 0U &&
         result.estimated_presentation_timestamp_us >=
             result.write_completed_timestamp_us &&
         result.queued_sample_frames_before_write <=
             RenderReferenceFrame48k::kMaximumQueuedSampleFrames &&
         timing_source_valid;
}

PlaybackSubmitCode MapSubmitGenerationDecision(
    const PlaybackGenerationDecision decision) noexcept {
  switch (decision) {
    case PlaybackGenerationDecision::kAccepted:
      return PlaybackSubmitCode::kAccepted;
    case PlaybackGenerationDecision::kEpochMismatch:
      return PlaybackSubmitCode::kEpochMismatch;
    case PlaybackGenerationDecision::kStreamMismatch:
      return PlaybackSubmitCode::kStreamMismatch;
    case PlaybackGenerationDecision::kTurnMismatch:
      return PlaybackSubmitCode::kTurnMismatch;
    case PlaybackGenerationDecision::kInvalidGeneration:
    case PlaybackGenerationDecision::kNotActive:
      return PlaybackSubmitCode::kCancellationRequired;
  }
  return PlaybackSubmitCode::kCancellationRequired;
}

PlaybackCommitResult MakeCommitResult(
    const PlaybackCommitCode code,
    const std::uint16_t accepted_sample_frames,
    const std::uint16_t accepted_from_current_chunk,
    const std::uint16_t remaining_in_current_chunk,
    const std::uint64_t generation_accepted_sample_frames,
    const std::uint64_t accepted_chunk_sequence,
    const bool chunk_completed = false,
    const bool end_of_stream_accepted = false,
    const bool cancellation_observed_after_accept = false,
    const std::int32_t native_error = 0) noexcept {
  return {code,
          accepted_sample_frames,
          accepted_from_current_chunk,
          remaining_in_current_chunk,
          generation_accepted_sample_frames,
          accepted_chunk_sequence,
          chunk_completed,
          end_of_stream_accepted,
          cancellation_observed_after_accept,
          native_error};
}

PlaybackCancelResult MakeCancelResult(
    const PlaybackCancelCode code, const std::uint64_t request_id,
    const PlaybackGeneration& generation) noexcept {
  return {code, request_id, generation, 0U, 0U, 0U, 0U, 0U,
          0U,   0U,         0,          false, false, false, false};
}

}  // namespace

Status PlaybackCommitter::Create(
    const PlaybackCommitterConfig& config, PlaybackEpochFence* const fence,
    PcmPlaybackSink* const sink, AcceptedRenderQueue* const accepted_queue,
    PlaybackCommitter* const output) {
  if (output == nullptr) {
    return Status::Error(StatusCode::kInvalidArgument,
                         "playback committer output must not be null");
  }
  if (output->state_ != PlaybackCommitterState::kUnconfigured) {
    return Status::Error(StatusCode::kFailedPrecondition,
                         "playback committer is already configured");
  }
  if (!IsKnownReferencePolicy(config.reference_policy)) {
    return Status::Error(StatusCode::kInvalidArgument,
                         "playback reference policy is invalid");
  }
  if (config.initial_pcm_incarnation == 0U ||
      config.first_accepted_chunk_sequence == 0U) {
    return Status::Error(
        StatusCode::kInvalidArgument,
        "playback committer identities must be non-zero");
  }
  if (fence == nullptr || sink == nullptr) {
    return Status::Error(
        StatusCode::kInvalidArgument,
        "playback committer fence and sink must not be null");
  }
  if (config.reference_policy ==
          PlaybackReferencePolicy::kRequiredSoftwareReference &&
      accepted_queue == nullptr) {
    return Status::Error(
        StatusCode::kInvalidArgument,
        "software playback reference requires an accepted queue");
  }

  output->config_ = config;
  output->fence_ = fence;
  output->sink_ = sink;
  output->accepted_queue_ = accepted_queue;
  output->next_accepted_chunk_sequence_ =
      config.first_accepted_chunk_sequence;
  output->state_ = PlaybackCommitterState::kUnprepared;
  return Status::Ok();
}

Status PlaybackCommitter::Initialize() {
  if (state_ == PlaybackCommitterState::kUnconfigured) {
    return Status::Error(StatusCode::kFailedPrecondition,
                         "playback committer is not configured");
  }
  if (state_ != PlaybackCommitterState::kUnprepared) {
    return Status::Error(StatusCode::kFailedPrecondition,
                         "playback committer is already initialized");
  }

  const PcmPlaybackControlResult prepare_result = sink_->Prepare();
  if (!prepare_result.valid() || !prepare_result.succeeded()) {
    state_ = PlaybackCommitterState::kSinkFault;
    return Status::Error(StatusCode::kInternal,
                         "playback sink initial prepare failed");
  }

  pcm_incarnation_ = config_.initial_pcm_incarnation;
  state_ = PlaybackCommitterState::kPreparedIdle;
  return Status::Ok();
}

Status PlaybackCommitter::Arm(const PlaybackGeneration& generation) {
  if (!generation.valid()) {
    return Status::Error(StatusCode::kInvalidArgument,
                         "playback committer generation must be complete");
  }
  if (state_ == PlaybackCommitterState::kSinkFault) {
    return Status::Error(StatusCode::kFailedPrecondition,
                         "playback committer sink is faulted");
  }
  if (accepted_sequence_exhausted_ ||
      next_accepted_chunk_sequence_ == 0U) {
    return Status::Error(
        StatusCode::kFailedPrecondition,
        "playback accepted identity exhausted; runtime restart required");
  }
  if (state_ != PlaybackCommitterState::kPreparedIdle) {
    return Status::Error(StatusCode::kFailedPrecondition,
                         "playback committer is not prepared and idle");
  }

  const std::uint32_t observed_epoch = fence_->Observe();
  const Status gate_status =
      generation_gate_.Activate(generation, observed_epoch);
  if (!gate_status.ok()) {
    return gate_status;
  }

  // Close the Activate-to-ACK race. No data has been submitted yet, so a
  // changed fence can roll back without a sink drop.
  const std::uint32_t confirmed_epoch = fence_->Observe();
  if (confirmed_epoch != generation.epoch) {
    (void)generation_gate_.InvalidateIfEpochChanged(confirmed_epoch);
    return Status::Error(
        StatusCode::kFailedPrecondition,
        "playback fence changed while arming the committer");
  }

  active_generation_ = generation;
  generation_accepted_sample_frames_ = 0U;
  accepted_after_cancel_fence_sample_frames_ = 0U;
  last_accepted_chunk_sequence_ = 0U;
  ClearPendingChunk();
  last_completed_metadata_ = AudioFrameMetadata{};
  last_completed_source_offset_ = 0U;
  last_completed_valid_samples_ = 0U;
  last_completed_end_of_stream_ = false;
  order_initialized_ = false;
  state_ = PlaybackCommitterState::kArmedIdle;
  return Status::Ok();
}

PlaybackSubmitResult PlaybackCommitter::Submit(
    const PlaybackPcmFrame48k& frame) noexcept {
  switch (state_) {
    case PlaybackCommitterState::kUnconfigured:
      return {PlaybackSubmitCode::kNotConfigured};
    case PlaybackCommitterState::kUnprepared:
    case PlaybackCommitterState::kPreparedIdle:
      return {PlaybackSubmitCode::kNotPrepared};
    case PlaybackCommitterState::kChunkPending:
      return {PlaybackSubmitCode::kChunkAlreadyPending};
    case PlaybackCommitterState::kEndOfStreamAccepted:
      return {PlaybackSubmitCode::kEndOfStreamAlreadyAccepted};
    case PlaybackCommitterState::kCancellationRequired:
      return {PlaybackSubmitCode::kCancellationRequired};
    case PlaybackCommitterState::kAwaitingReferenceReset:
      return {PlaybackSubmitCode::kNotPrepared};
    case PlaybackCommitterState::kSinkFault:
      return {PlaybackSubmitCode::kSinkFault};
    case PlaybackCommitterState::kArmedIdle:
      break;
  }

  if (accepted_sequence_exhausted_ ||
      next_accepted_chunk_sequence_ == 0U) {
    accepted_sequence_exhausted_ = true;
    RequireCancellation();
    return {PlaybackSubmitCode::kRestartRequired};
  }

  if (!frame.HasValidLength() || frame.metadata.discontinuity) {
    RequireCancellation();
    return {PlaybackSubmitCode::kInvalidFrame};
  }

  const PlaybackGeneration frame_generation = GenerationOf(frame);
  if (frame_generation.epoch != active_generation_.epoch) {
    return {PlaybackSubmitCode::kEpochMismatch};
  }
  if (frame_generation.stream_id != active_generation_.stream_id) {
    return {PlaybackSubmitCode::kStreamMismatch};
  }
  if (frame_generation.turn_id != active_generation_.turn_id) {
    return {PlaybackSubmitCode::kTurnMismatch};
  }

  const PlaybackGenerationDecision generation_decision =
      RefreshGenerationGate();
  if (generation_decision != PlaybackGenerationDecision::kAccepted) {
    RequireCancellation();
    return {MapSubmitGenerationDecision(generation_decision)};
  }

  const PlaybackSubmitCode order_result = ValidateSubmitOrder(frame);
  if (order_result != PlaybackSubmitCode::kAccepted) {
    RequireCancellation();
    return {order_result};
  }

  pending_frame_ = frame;
  pending_cursor_ = 0U;
  state_ = PlaybackCommitterState::kChunkPending;
  return {PlaybackSubmitCode::kAccepted};
}

PlaybackCommitResult PlaybackCommitter::PumpOnce() noexcept {
  if (state_ == PlaybackCommitterState::kUnconfigured) {
    return MakeCommitResult(PlaybackCommitCode::kNotConfigured, 0U, 0U, 0U,
                            0U, 0U);
  }
  if (state_ == PlaybackCommitterState::kSinkFault) {
    return MakeCommitResult(PlaybackCommitCode::kSinkFault, 0U, 0U, 0U,
                            generation_accepted_sample_frames_,
                            last_accepted_chunk_sequence_);
  }
  if (state_ == PlaybackCommitterState::kCancellationRequired) {
    const std::uint16_t remaining =
        pending_frame_.valid_samples >= pending_cursor_
            ? static_cast<std::uint16_t>(pending_frame_.valid_samples -
                                         pending_cursor_)
            : 0U;
    return MakeCommitResult(
        PlaybackCommitCode::kCancellationRequiredBeforeWrite, 0U,
        pending_cursor_, remaining, generation_accepted_sample_frames_,
        last_accepted_chunk_sequence_);
  }
  if (state_ == PlaybackCommitterState::kAwaitingReferenceReset) {
    return MakeCommitResult(PlaybackCommitCode::kNotArmed, 0U, 0U, 0U, 0U,
                            last_cancel_result_.last_accepted_chunk_sequence);
  }
  if (state_ == PlaybackCommitterState::kEndOfStreamAccepted) {
    return MakeCommitResult(
        PlaybackCommitCode::kEndOfStreamAlreadyAccepted, 0U, 0U, 0U,
        generation_accepted_sample_frames_, last_accepted_chunk_sequence_);
  }
  if (state_ == PlaybackCommitterState::kUnprepared ||
      state_ == PlaybackCommitterState::kPreparedIdle ||
      state_ == PlaybackCommitterState::kArmedIdle) {
    return MakeCommitResult(
        state_ == PlaybackCommitterState::kArmedIdle
            ? PlaybackCommitCode::kNoChunkPending
            : PlaybackCommitCode::kNotArmed,
        0U, 0U, 0U, generation_accepted_sample_frames_,
        last_accepted_chunk_sequence_);
  }

  const std::uint16_t remaining_before = static_cast<std::uint16_t>(
      pending_frame_.valid_samples - pending_cursor_);
  if (RefreshGenerationGate() != PlaybackGenerationDecision::kAccepted) {
    RequireCancellation();
    return MakeCommitResult(
        PlaybackCommitCode::kCancellationRequiredBeforeWrite, 0U,
        pending_cursor_, remaining_before, generation_accepted_sample_frames_,
        last_accepted_chunk_sequence_);
  }

  AcceptedRenderQueue::ProducerLease accepted_lease{};
  const bool reference_required =
      config_.reference_policy ==
      PlaybackReferencePolicy::kRequiredSoftwareReference;
  if (reference_required) {
    accepted_lease = accepted_queue_->TryAcquireWrite();
    if (!accepted_lease) {
      return MakeCommitResult(
          PlaybackCommitCode::kReferenceBackpressure, 0U, pending_cursor_,
          remaining_before, generation_accepted_sample_frames_,
          last_accepted_chunk_sequence_);
    }
  }

  // This second observation is intentionally adjacent to the nonblocking
  // write. A fence can still change inside the syscall window; a positive
  // return is recorded exactly once below before cancellation is required.
  if (RefreshGenerationGate() != PlaybackGenerationDecision::kAccepted) {
    RequireCancellation();
    return MakeCommitResult(
        PlaybackCommitCode::kCancellationRequiredBeforeWrite, 0U,
        pending_cursor_, remaining_before, generation_accepted_sample_frames_,
        last_accepted_chunk_sequence_);
  }
  if (accepted_sequence_exhausted_ ||
      next_accepted_chunk_sequence_ == 0U) {
    accepted_sequence_exhausted_ = true;
    RequireCancellation();
    return MakeCommitResult(PlaybackCommitCode::kRestartRequired, 0U,
                            pending_cursor_, remaining_before,
                            generation_accepted_sample_frames_,
                            last_accepted_chunk_sequence_);
  }
  if (generation_accepted_sample_frames_ >
          std::numeric_limits<std::uint64_t>::max() - remaining_before) {
    RequireCancellation();
    return MakeCommitResult(PlaybackCommitCode::kSinkProtocolFault, 0U,
                            pending_cursor_, remaining_before,
                            generation_accepted_sample_frames_,
                            last_accepted_chunk_sequence_);
  }

  const std::uint16_t cursor_before = pending_cursor_;
  const PcmPlaybackWriteResult write_result =
      sink_->TryWriteInterleavedS16(
          pending_frame_.samples.data() + cursor_before, remaining_before);

  const auto make_no_progress_result =
      [&](const PlaybackCommitCode code) noexcept {
        return MakeCommitResult(
            code, 0U, pending_cursor_, remaining_before,
            generation_accepted_sample_frames_,
            last_accepted_chunk_sequence_, false, false, false,
            write_result.native_error);
      };

  if (write_result.accepted_sample_frames == 0U) {
    if (!write_result.valid() ||
        write_result.code == PcmPlaybackWriteCode::kAccepted) {
      RequireCancellation();
      return make_no_progress_result(
          PlaybackCommitCode::kSinkProtocolFault);
    }
    switch (write_result.code) {
      case PcmPlaybackWriteCode::kWouldBlock:
        return make_no_progress_result(PlaybackCommitCode::kWouldBlock);
      case PcmPlaybackWriteCode::kInterrupted:
        return make_no_progress_result(PlaybackCommitCode::kInterrupted);
      case PcmPlaybackWriteCode::kXrun:
        RequireCancellation();
        return make_no_progress_result(PlaybackCommitCode::kXrun);
      case PcmPlaybackWriteCode::kSuspended:
        RequireCancellation();
        return make_no_progress_result(PlaybackCommitCode::kSuspended);
      case PcmPlaybackWriteCode::kDeviceLost:
        RequireCancellation();
        return make_no_progress_result(PlaybackCommitCode::kDeviceLost);
      case PcmPlaybackWriteCode::kZeroProgress:
        RequireCancellation();
        return make_no_progress_result(PlaybackCommitCode::kZeroProgress);
      case PcmPlaybackWriteCode::kFatal:
      case PcmPlaybackWriteCode::kUnavailable:
        RequireCancellation();
        return make_no_progress_result(PlaybackCommitCode::kSinkFault);
      case PcmPlaybackWriteCode::kAccepted:
        break;
    }
    RequireCancellation();
    return make_no_progress_result(PlaybackCommitCode::kSinkProtocolFault);
  }

  // Any positive count is irreversible even if the adapter returned a
  // contradictory code or malformed ancillary timing. Clamp an impossible
  // over-report to the requested span so this process never replays samples
  // that may already have been accepted by the device.
  const std::uint16_t reported_accepted =
      write_result.accepted_sample_frames;
  const std::uint16_t accepted =
      reported_accepted <= remaining_before ? reported_accepted
                                            : remaining_before;
  const bool accepted_result_valid =
      write_result.code == PcmPlaybackWriteCode::kAccepted &&
      reported_accepted <= remaining_before && write_result.valid() &&
      IsValidTiming(write_result);

  // Positive return is irreversible. Advance the local cursor before any
  // later validation or publication so no adapter fault can replay it.
  pending_cursor_ = static_cast<std::uint16_t>(pending_cursor_ + accepted);
  generation_accepted_sample_frames_ += accepted;
  const bool chunk_completed =
      pending_cursor_ == pending_frame_.valid_samples;
  const bool source_end_of_stream =
      chunk_completed && pending_frame_.end_of_stream;
  const std::uint64_t accepted_sequence = next_accepted_chunk_sequence_;
  last_accepted_chunk_sequence_ = accepted_sequence;
  if (accepted_sequence == std::numeric_limits<std::uint64_t>::max()) {
    next_accepted_chunk_sequence_ = 0U;
    accepted_sequence_exhausted_ = true;
  } else {
    next_accepted_chunk_sequence_ = accepted_sequence + 1U;
  }

  const std::uint32_t epoch_after_accept = fence_->Observe();
  (void)generation_gate_.InvalidateIfEpochChanged(epoch_after_accept);
  bool cancellation_observed =
      epoch_after_accept != active_generation_.epoch ||
      generation_gate_.Check(active_generation_) !=
          PlaybackGenerationDecision::kAccepted;

  if (!accepted_result_valid) {
    const std::uint32_t epoch_after_validation = fence_->Observe();
    (void)generation_gate_.InvalidateIfEpochChanged(
        epoch_after_validation);
    cancellation_observed =
        cancellation_observed ||
        epoch_after_validation != active_generation_.epoch ||
        generation_gate_.Check(active_generation_) !=
            PlaybackGenerationDecision::kAccepted;
    if (cancellation_observed) {
      accepted_after_cancel_fence_sample_frames_ += accepted;
    }
    RequireCancellation();
    const std::uint16_t remaining_after = static_cast<std::uint16_t>(
        pending_frame_.valid_samples - pending_cursor_);
    return MakeCommitResult(
        PlaybackCommitCode::kAcceptedThenSinkProtocolFault, accepted,
        pending_cursor_, remaining_after, generation_accepted_sample_frames_,
        accepted_sequence, chunk_completed, source_end_of_stream,
        cancellation_observed, write_result.native_error);
  }

  if (reference_required) {
    AcceptedRenderChunk48k& accepted_chunk = accepted_lease.frame();
    accepted_chunk.samples.fill(0);
    for (std::size_t sample = 0U; sample < accepted; ++sample) {
      accepted_chunk.samples[sample] =
          pending_frame_.samples[static_cast<std::size_t>(cursor_before) +
                                 sample];
    }
    accepted_chunk.format = pending_frame_.format;
    accepted_chunk.metadata = pending_frame_.metadata;
    accepted_chunk.pcm_incarnation = pcm_incarnation_;
    accepted_chunk.accepted_chunk_sequence = accepted_sequence;
    accepted_chunk.absolute_source_offset_sample_frames =
        static_cast<std::uint16_t>(
            pending_frame_.source_offset_sample_frames + cursor_before);
    accepted_chunk.accepted_samples = accepted;
    accepted_chunk.completes_render_chunk = chunk_completed;
    accepted_chunk.source_end_of_stream = source_end_of_stream;
    accepted_chunk.write_completed_timestamp_us =
        write_result.write_completed_timestamp_us;
    accepted_chunk.estimated_presentation_timestamp_us =
        write_result.estimated_presentation_timestamp_us;
    accepted_chunk.queued_sample_frames_before_write =
        write_result.queued_sample_frames_before_write;
    accepted_chunk.timing_source = write_result.timing_source;
    if (!accepted_lease.Publish()) {
      RequireCancellation();
      const std::uint16_t remaining_after = static_cast<std::uint16_t>(
          pending_frame_.valid_samples - pending_cursor_);
      return MakeCommitResult(
          PlaybackCommitCode::kReferencePublishFault, accepted,
          pending_cursor_, remaining_after,
          generation_accepted_sample_frames_, accepted_sequence,
          chunk_completed, source_end_of_stream, cancellation_observed,
          write_result.native_error);
    }
  }

  // A cancel may arrive after the first post-write observation while the
  // accepted fact is being copied/published. Recheck before declaring the
  // chunk stable. The accepted record deliberately retains the old
  // generation so the reference consumer can reject it at apply time.
  const std::uint32_t epoch_after_publish = fence_->Observe();
  (void)generation_gate_.InvalidateIfEpochChanged(epoch_after_publish);
  cancellation_observed =
      cancellation_observed ||
      epoch_after_publish != active_generation_.epoch ||
      generation_gate_.Check(active_generation_) !=
          PlaybackGenerationDecision::kAccepted;

  const std::uint16_t remaining_after = static_cast<std::uint16_t>(
      pending_frame_.valid_samples - pending_cursor_);
  if (cancellation_observed) {
    accepted_after_cancel_fence_sample_frames_ += accepted;
    RequireCancellation();
    return MakeCommitResult(
        PlaybackCommitCode::kAcceptedThenCancellationRequired, accepted,
        pending_cursor_, remaining_after, generation_accepted_sample_frames_,
        accepted_sequence, chunk_completed, source_end_of_stream, true,
        write_result.native_error);
  }

  if (!chunk_completed) {
    return MakeCommitResult(
        PlaybackCommitCode::kAcceptedPartial, accepted, pending_cursor_,
        remaining_after, generation_accepted_sample_frames_, accepted_sequence,
        false, false, false, write_result.native_error);
  }

  RecordCompletedChunk(pending_frame_);
  ClearPendingChunk();
  state_ = source_end_of_stream
               ? PlaybackCommitterState::kEndOfStreamAccepted
               : PlaybackCommitterState::kArmedIdle;
  return MakeCommitResult(
      source_end_of_stream ? PlaybackCommitCode::kEndOfStreamAccepted
                           : PlaybackCommitCode::kChunkAccepted,
      accepted, static_cast<std::uint16_t>(cursor_before + accepted), 0U,
      generation_accepted_sample_frames_, accepted_sequence, true,
      source_end_of_stream, false, write_result.native_error);
}

PlaybackCancelResult PlaybackCommitter::Cancel(
    const std::uint64_t request_id,
    const PlaybackGeneration& expected_generation) noexcept {
  if (has_cached_cancel_result_ &&
      request_id == last_cancel_result_.request_id &&
      GenerationsEqual(expected_generation, last_cancel_result_.generation) &&
      last_cancel_result_.acknowledged) {
    PlaybackCancelResult duplicate = last_cancel_result_;
    duplicate.code =
        last_cancel_result_.code == PlaybackCancelCode::kRestartRequired
            ? PlaybackCancelCode::kRestartRequired
            : PlaybackCancelCode::kDuplicateAcknowledged;
    return duplicate;
  }
  if (state_ == PlaybackCommitterState::kUnconfigured) {
    return MakeCancelResult(PlaybackCancelCode::kNotConfigured, request_id,
                            expected_generation);
  }
  if (request_id == 0U || !expected_generation.valid()) {
    return MakeCancelResult(PlaybackCancelCode::kInvalidRequest, request_id,
                            expected_generation);
  }
  if (state_ == PlaybackCommitterState::kSinkFault) {
    return MakeCancelResult(PlaybackCancelCode::kSinkFault, request_id,
                            expected_generation);
  }
  if (!active_generation_.valid()) {
    return MakeCancelResult(PlaybackCancelCode::kNotActive, request_id,
                            expected_generation);
  }
  if (!GenerationsEqual(active_generation_, expected_generation)) {
    return MakeCancelResult(PlaybackCancelCode::kGenerationMismatch,
                            request_id, expected_generation);
  }

  PlaybackCancelResult result =
      MakeCancelResult(PlaybackCancelCode::kFenceNotAdvanced, request_id,
                       expected_generation);
  result.observed_cancel_epoch = fence_->Observe();
  result.retired_pcm_incarnation = pcm_incarnation_;
  result.accepted_generation_sample_frames =
      generation_accepted_sample_frames_;
  result.last_accepted_chunk_sequence = last_accepted_chunk_sequence_;
  result.accepted_after_cancel_fence_sample_frames =
      accepted_after_cancel_fence_sample_frames_;
  result.discarded_pending_sample_frames =
      pending_frame_.valid_samples >= pending_cursor_
          ? static_cast<std::uint16_t>(pending_frame_.valid_samples -
                                       pending_cursor_)
          : 0U;
  if (result.observed_cancel_epoch == 0U ||
      result.observed_cancel_epoch == expected_generation.epoch) {
    return result;
  }

  (void)generation_gate_.InvalidateIfEpochChanged(
      result.observed_cancel_epoch);
  ClearPendingChunk();
  state_ = PlaybackCommitterState::kCancellationRequired;
  result.reference_reset_required = true;

  const PcmPlaybackControlResult drop_result = sink_->Drop();
  result.native_error = drop_result.native_error;
  if (!drop_result.valid() || !drop_result.succeeded()) {
    result.code = PlaybackCancelCode::kDropFailed;
    state_ = PlaybackCommitterState::kSinkFault;
    return result;
  }
  result.drop_succeeded = true;

  if (pcm_incarnation_ == std::numeric_limits<std::uint64_t>::max()) {
    result.code = PlaybackCancelCode::kIncarnationExhausted;
    state_ = PlaybackCommitterState::kSinkFault;
    return result;
  }
  ++pcm_incarnation_;

  const PcmPlaybackControlResult prepare_result = sink_->Prepare();
  result.native_error = prepare_result.native_error;
  if (!prepare_result.valid() || !prepare_result.succeeded()) {
    result.code = PlaybackCancelCode::kPrepareFailed;
    state_ = PlaybackCommitterState::kSinkFault;
    return result;
  }
  result.prepare_succeeded = true;
  result.prepared_pcm_incarnation = pcm_incarnation_;
  result.acknowledged = true;
  result.code = PlaybackCancelCode::kAcknowledged;

  const bool restart_required = accepted_sequence_exhausted_;
  ResetGenerationState();
  state_ = restart_required ? PlaybackCommitterState::kSinkFault
                            : PlaybackCommitterState::kAwaitingReferenceReset;
  if (restart_required) {
    result.code = PlaybackCancelCode::kRestartRequired;
  }
  last_cancel_result_ = result;
  has_cached_cancel_result_ = true;
  reference_reset_confirmed_ = false;
  return result;
}

PlaybackReferenceResetResult PlaybackCommitter::ConfirmReferenceReset(
    const std::uint64_t request_id,
    const PlaybackGeneration& generation) noexcept {
  if (reference_reset_confirmed_ && has_cached_cancel_result_ &&
      request_id == last_cancel_result_.request_id &&
      GenerationsEqual(generation, last_cancel_result_.generation)) {
    return {PlaybackReferenceResetCode::kDuplicateConfirmed, request_id,
            generation, true};
  }
  if (request_id == 0U || !generation.valid()) {
    return {PlaybackReferenceResetCode::kInvalidConfirmation, request_id,
            generation, false};
  }
  if (state_ == PlaybackCommitterState::kSinkFault) {
    return {PlaybackReferenceResetCode::kSinkFault, request_id, generation,
            false};
  }
  if (state_ != PlaybackCommitterState::kAwaitingReferenceReset ||
      !has_cached_cancel_result_ || !last_cancel_result_.acknowledged) {
    return {PlaybackReferenceResetCode::kNotAwaiting, request_id, generation,
            false};
  }
  if (request_id != last_cancel_result_.request_id) {
    return {PlaybackReferenceResetCode::kRequestMismatch, request_id,
            generation, false};
  }
  if (!GenerationsEqual(generation, last_cancel_result_.generation)) {
    return {PlaybackReferenceResetCode::kGenerationMismatch, request_id,
            generation, false};
  }

  reference_reset_confirmed_ = true;
  state_ = PlaybackCommitterState::kPreparedIdle;
  return {PlaybackReferenceResetCode::kConfirmed, request_id, generation,
          true};
}

PlaybackGenerationDecision PlaybackCommitter::RefreshGenerationGate()
    noexcept {
  const std::uint32_t observed_epoch = fence_->Observe();
  (void)generation_gate_.InvalidateIfEpochChanged(observed_epoch);
  return generation_gate_.Check(active_generation_);
}

PlaybackSubmitCode PlaybackCommitter::ValidateSubmitOrder(
    const PlaybackPcmFrame48k& frame) const noexcept {
  if (!order_initialized_) {
    return frame.source_offset_sample_frames == 0U
               ? PlaybackSubmitCode::kAccepted
               : PlaybackSubmitCode::kOrderingFault;
  }
  if (last_completed_end_of_stream_) {
    return PlaybackSubmitCode::kOrderingFault;
  }

  if (frame.metadata.sequence == last_completed_metadata_.sequence) {
    const auto expected_offset = static_cast<std::uint32_t>(
        last_completed_source_offset_) + last_completed_valid_samples_;
    return MetadataEqual(frame.metadata, last_completed_metadata_) &&
                   expected_offset == frame.source_offset_sample_frames &&
                   frame.end_of_stream
               ? PlaybackSubmitCode::kAccepted
               : PlaybackSubmitCode::kOrderingFault;
  }

  if (last_completed_metadata_.sequence ==
          std::numeric_limits<std::uint32_t>::max() ||
      frame.metadata.sequence != last_completed_metadata_.sequence + 1U ||
      frame.metadata.monotonic_timestamp_us <=
          last_completed_metadata_.monotonic_timestamp_us ||
      frame.source_offset_sample_frames != 0U) {
    return PlaybackSubmitCode::kOrderingFault;
  }
  return PlaybackSubmitCode::kAccepted;
}

void PlaybackCommitter::RecordCompletedChunk(
    const PlaybackPcmFrame48k& frame) noexcept {
  last_completed_metadata_ = frame.metadata;
  last_completed_source_offset_ = frame.source_offset_sample_frames;
  last_completed_valid_samples_ = frame.valid_samples;
  last_completed_end_of_stream_ = frame.end_of_stream;
  order_initialized_ = true;
}

void PlaybackCommitter::ClearPendingChunk() noexcept {
  pending_frame_.ResetHeader();
  pending_cursor_ = 0U;
}

void PlaybackCommitter::ResetGenerationState() noexcept {
  active_generation_ = PlaybackGeneration{};
  generation_accepted_sample_frames_ = 0U;
  accepted_after_cancel_fence_sample_frames_ = 0U;
  last_accepted_chunk_sequence_ = 0U;
  ClearPendingChunk();
  last_completed_metadata_ = AudioFrameMetadata{};
  last_completed_source_offset_ = 0U;
  last_completed_valid_samples_ = 0U;
  last_completed_end_of_stream_ = false;
  order_initialized_ = false;
}

void PlaybackCommitter::RequireCancellation() noexcept {
  if (state_ != PlaybackCommitterState::kSinkFault) {
    state_ = PlaybackCommitterState::kCancellationRequired;
  }
}

}  // namespace boompi::audio
