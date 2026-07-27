#ifndef BOOMPI_AUDIO_PLAYBACK_COMMITTER_H_
#define BOOMPI_AUDIO_PLAYBACK_COMMITTER_H_

#include <cstdint>

#include "boompi/audio/pcm_playback_sink.h"
#include "boompi/audio/playback_buffer.h"
#include "boompi/audio/playback_frame.h"
#include "boompi/audio/playback_generation.h"
#include "boompi/event/status.h"

namespace boompi::audio {

enum class PlaybackReferencePolicy : std::uint8_t {
  kRequiredSoftwareReference = 0,
  kHardwareCaptureReference,
};

struct PlaybackCommitterConfig final {
  PlaybackReferencePolicy reference_policy{
      PlaybackReferencePolicy::kRequiredSoftwareReference};
  // The runtime owner allocates both identities without reuse if it ever
  // rebuilds a committer in the same process/session.
  std::uint64_t initial_pcm_incarnation{1U};
  std::uint64_t first_accepted_chunk_sequence{1U};
};

enum class PlaybackCommitterState : std::uint8_t {
  kUnconfigured = 0,
  kUnprepared,
  kPreparedIdle,
  kArmedIdle,
  kChunkPending,
  // The final EOS chunk was accepted by the sink, but accepted is not proof
  // of presentation. The generation remains cancellable in this state.
  kEndOfStreamAccepted,
  kCancellationRequired,
  // Local Drop+Prepare succeeded. Arm remains forbidden until the actor
  // supplies the exact DSP/reference reset ACK through ConfirmReferenceReset.
  kAwaitingReferenceReset,
  kSinkFault,
};

enum class PlaybackSubmitCode : std::uint8_t {
  kUnset = 0,
  kAccepted,
  kNotConfigured,
  kNotPrepared,
  kNotArmed,
  kChunkAlreadyPending,
  kEndOfStreamAlreadyAccepted,
  kInvalidFrame,
  kEpochMismatch,
  kStreamMismatch,
  kTurnMismatch,
  kOrderingFault,
  kRestartRequired,
  kCancellationRequired,
  kSinkFault,
};

struct PlaybackSubmitResult final {
  PlaybackSubmitCode code;

  constexpr bool accepted() const noexcept {
    return code == PlaybackSubmitCode::kAccepted;
  }

  constexpr bool requires_cancel() const noexcept {
    return code == PlaybackSubmitCode::kInvalidFrame ||
           code == PlaybackSubmitCode::kOrderingFault ||
           code == PlaybackSubmitCode::kRestartRequired ||
           code == PlaybackSubmitCode::kCancellationRequired;
  }
};

enum class PlaybackCommitCode : std::uint8_t {
  kUnset = 0,
  kAcceptedPartial,
  kChunkAccepted,
  kEndOfStreamAccepted,
  kWouldBlock,
  kInterrupted,
  kReferenceBackpressure,
  kNotConfigured,
  kNotArmed,
  kNoChunkPending,
  kEndOfStreamAlreadyAccepted,
  kCancellationRequiredBeforeWrite,
  kAcceptedThenCancellationRequired,
  kAcceptedThenSinkProtocolFault,
  kXrun,
  kSuspended,
  kDeviceLost,
  kZeroProgress,
  kSinkProtocolFault,
  kReferencePublishFault,
  kRestartRequired,
  kSinkFault,
};

struct PlaybackCommitResult final {
  PlaybackCommitCode code;
  std::uint16_t accepted_sample_frames;
  std::uint16_t accepted_from_current_chunk;
  std::uint16_t remaining_in_current_chunk;
  std::uint64_t generation_accepted_sample_frames;
  std::uint64_t accepted_chunk_sequence;
  bool chunk_completed;
  bool end_of_stream_accepted;
  bool cancellation_observed_after_accept;
  // Backend-native detail retained only for bounded diagnostics. Core policy
  // must never branch on this value.
  std::int32_t native_error;

  constexpr bool made_progress() const noexcept {
    return accepted_sample_frames != 0U;
  }

  constexpr bool requires_cancel() const noexcept {
    return code == PlaybackCommitCode::kCancellationRequiredBeforeWrite ||
           code == PlaybackCommitCode::kAcceptedThenCancellationRequired ||
           code == PlaybackCommitCode::kAcceptedThenSinkProtocolFault ||
           code == PlaybackCommitCode::kXrun ||
           code == PlaybackCommitCode::kSuspended ||
           code == PlaybackCommitCode::kDeviceLost ||
           code == PlaybackCommitCode::kZeroProgress ||
           code == PlaybackCommitCode::kSinkProtocolFault ||
           code == PlaybackCommitCode::kReferencePublishFault ||
           code == PlaybackCommitCode::kRestartRequired ||
           code == PlaybackCommitCode::kSinkFault;
  }
};

enum class PlaybackCancelCode : std::uint8_t {
  kUnset = 0,
  kAcknowledged,
  kDuplicateAcknowledged,
  kNotConfigured,
  kInvalidRequest,
  kNotActive,
  kGenerationMismatch,
  kFenceNotAdvanced,
  kDropFailed,
  kPrepareFailed,
  kIncarnationExhausted,
  kRestartRequired,
  kSinkFault,
};

// This is only the playback worker's local ACK. Even an acknowledged result
// does not prove acoustic silence or that queued/held AEC reference has been
// discarded. The actor must separately join it with a DSP reference-reset
// ACK before authorizing a new software-reference generation.
struct PlaybackCancelResult final {
  PlaybackCancelCode code;
  std::uint64_t request_id;
  PlaybackGeneration generation;
  std::uint32_t observed_cancel_epoch;
  // Drop retires the first identity. The second stays zero unless Prepare
  // proves that the new PCM timeline is ready.
  std::uint64_t retired_pcm_incarnation;
  std::uint64_t prepared_pcm_incarnation;
  std::uint64_t accepted_generation_sample_frames;
  std::uint64_t last_accepted_chunk_sequence;
  std::uint32_t accepted_after_cancel_fence_sample_frames;
  std::uint16_t discarded_pending_sample_frames;
  std::int32_t native_error;
  bool drop_succeeded;
  bool prepare_succeeded;
  bool reference_reset_required;
  bool acknowledged;
};

enum class PlaybackReferenceResetCode : std::uint8_t {
  kUnset = 0,
  kConfirmed,
  kDuplicateConfirmed,
  kInvalidConfirmation,
  kNotAwaiting,
  kRequestMismatch,
  kGenerationMismatch,
  kSinkFault,
};

struct PlaybackReferenceResetResult final {
  PlaybackReferenceResetCode code;
  std::uint64_t request_id;
  PlaybackGeneration generation;
  bool confirmed;
};

// Portable single-worker commit core. It owns no thread and never blocks.
// Create/Initialize/Arm are cold-path calls. Submit, PumpOnce, and Cancel use
// fixed member storage, perform no allocation or logging, and retain no queue
// lease or caller pointer across calls.
//
// A positive sink write result is the accepted linearization point. In
// required-software-reference mode PumpOnce reserves an AcceptedRenderQueue
// lease before every write; queue backpressure therefore causes zero sink
// writes. Accepted chunks are factual accepted prefixes, not proof of DAC or
// audible presentation.
class PlaybackCommitter final {
 public:
  PlaybackCommitter() noexcept = default;
  PlaybackCommitter(const PlaybackCommitter&) = delete;
  PlaybackCommitter& operator=(const PlaybackCommitter&) = delete;
  PlaybackCommitter(PlaybackCommitter&&) = delete;
  PlaybackCommitter& operator=(PlaybackCommitter&&) = delete;

  static Status Create(const PlaybackCommitterConfig& config,
                       PlaybackEpochFence* fence,
                       PcmPlaybackSink* sink,
                       AcceptedRenderQueue* accepted_queue,
                       PlaybackCommitter* output);

  // Calls sink Prepare once and starts the configured PCM incarnation. A
  // failed initial prepare is fail-closed and requires rebuilding the
  // platform adapter.
  Status Initialize();
  Status Arm(const PlaybackGeneration& generation);

  PlaybackSubmitResult Submit(
      const PlaybackPcmFrame48k& frame) noexcept;

  // Makes at most one nonblocking sink write attempt.
  PlaybackCommitResult PumpOnce() noexcept;

  // The actor must AdvanceTo a newer fence epoch before calling Cancel. The
  // exact active generation check prevents a late old cancel from dropping a
  // newer stream. Success is idempotent for the same request id/generation.
  PlaybackCancelResult Cancel(
      std::uint64_t request_id,
      const PlaybackGeneration& expected_generation) noexcept;

  // Called only after the actor receives the DSP/AEC reference-reset ACK.
  // The exact request/generation check prevents a stale reset completion from
  // authorizing a new stream.
  PlaybackReferenceResetResult ConfirmReferenceReset(
      std::uint64_t request_id,
      const PlaybackGeneration& generation) noexcept;

  PlaybackCommitterState state() const noexcept { return state_; }
  std::uint64_t pcm_incarnation() const noexcept {
    return pcm_incarnation_;
  }

 private:
  PlaybackGenerationDecision RefreshGenerationGate() noexcept;
  PlaybackSubmitCode ValidateSubmitOrder(
      const PlaybackPcmFrame48k& frame) const noexcept;
  void RecordCompletedChunk(const PlaybackPcmFrame48k& frame) noexcept;
  void ClearPendingChunk() noexcept;
  void ResetGenerationState() noexcept;
  void RequireCancellation() noexcept;

  PlaybackCommitterConfig config_{};
  PlaybackEpochFence* fence_{nullptr};
  PcmPlaybackSink* sink_{nullptr};
  AcceptedRenderQueue* accepted_queue_{nullptr};
  PlaybackGenerationGate generation_gate_{};
  PlaybackGeneration active_generation_{};
  PlaybackPcmFrame48k pending_frame_{};
  AudioFrameMetadata last_completed_metadata_{};
  PlaybackCancelResult last_cancel_result_{};
  std::uint64_t generation_accepted_sample_frames_{0U};
  std::uint64_t next_accepted_chunk_sequence_{1U};
  std::uint64_t last_accepted_chunk_sequence_{0U};
  std::uint32_t accepted_after_cancel_fence_sample_frames_{0U};
  std::uint64_t pcm_incarnation_{0U};
  std::uint16_t pending_cursor_{0U};
  std::uint16_t last_completed_source_offset_{0U};
  std::uint16_t last_completed_valid_samples_{0U};
  PlaybackCommitterState state_{PlaybackCommitterState::kUnconfigured};
  bool last_completed_end_of_stream_{false};
  bool order_initialized_{false};
  bool has_cached_cancel_result_{false};
  bool reference_reset_confirmed_{false};
  bool accepted_sequence_exhausted_{false};
};

}  // namespace boompi::audio

#endif  // BOOMPI_AUDIO_PLAYBACK_COMMITTER_H_
