#ifndef BOOMPI_AUDIO_PLAYBACK_WORKER_H_
#define BOOMPI_AUDIO_PLAYBACK_WORKER_H_

#include <cstdint>

#include "boompi/audio/playback_committer.h"
#include "boompi/audio/playback_control.h"
#include "boompi/audio/playback_renderer_24_to_48.h"
#include "boompi/event/status.h"

namespace boompi::audio {

struct PlaybackWorkerConfig final {
  PlaybackGainLimiterConfig gain_limiter{};
  PlaybackCommitterConfig committer{};
  // Shared by the EOS and critical-event lanes. Every allocated identity is
  // non-zero and process-unique; exhaustion is terminal rather than wrapping.
  std::uint64_t first_stream_event_sequence{1U};
};

// All endpoints outlive the worker. Each mailbox and audio queue still has
// exactly the producer/consumer ownership documented by its concrete type.
struct PlaybackWorkerEndpoints final {
  PlaybackEpochFence* fence{nullptr};
  PcmPlaybackSink* sink{nullptr};
  TtsIngressQueue* ingress{nullptr};
  AcceptedRenderQueue* accepted_queue{nullptr};
  PlaybackUrgentCancelMailbox* urgent_cancel{nullptr};
  PlaybackLifecycleCommandMailbox* lifecycle_commands{nullptr};
  PlaybackLocalCancelResultMailbox* local_cancel_results{nullptr};
  PlaybackLifecycleResultMailbox* lifecycle_results{nullptr};
  PlaybackEndOfStreamEventMailbox* eos_events{nullptr};
  PlaybackCriticalStreamEventMailbox* critical_events{nullptr};
};

enum class PlaybackWorkerState : std::uint8_t {
  kUnconfigured = 0,
  kUninitialized,
  kPreparedIdle,
  kArming,
  kActive,
  kCancellationRequired,
  kAwaitingIngressQuiescence,
  kAwaitingReferenceReset,
  kEndOfStreamAccepted,
  kStopped,
  kFaulted,
};

enum class PlaybackWorkerStepCode : std::uint8_t {
  kIdle = 0,
  kProgress,
  kOutputBackpressure,
  kSinkBackpressure,
  kAwaitingCancellation,
  // A command without a reply-correlatable identity was consumed and ignored.
  // No result mailbox entry is fabricated; the runner must diagnose this code.
  kInvalidControlInput,
  kNotInitialized,
  kStopped,
  kFaulted,
};

struct PlaybackWorkerStepResult final {
  PlaybackWorkerStepCode code{PlaybackWorkerStepCode::kIdle};
  PlaybackWorkerState state{PlaybackWorkerState::kUnconfigured};
};

// Synchronous playback-thread core whose hot data path uses fixed storage and
// has no per-frame allocation. An ordinary Step performs at most one bounded
// control, render, submit, drain, or nonblocking write. Urgent cancel is one
// fixed-upper-bound composite transaction: renderer Disarm, the initial
// 64-frame diagnostic drain, then sink Drop/Prepare. The core never owns a
// thread or retains an ingress ConsumerLease across a call. Urgent
// cancellation is inspected before every ordinary output retry. Cold control
// errors may construct Status diagnostics. A control payload with a complete
// reply identity receives a structurally valid typed error; an envelope whose
// request/generation cannot be correlated is consumed without fabricating an
// invalid mailbox result and is reported as kInvalidControlInput by Step.
// kActive becomes observable only after the Arm ACK is published. The final
// acknowledged ingress-quiesce result is cached for exact, side-effect-free
// replay.
class PlaybackWorkerCore final {
 public:
  PlaybackWorkerCore() noexcept = default;
  PlaybackWorkerCore(const PlaybackWorkerCore&) = delete;
  PlaybackWorkerCore& operator=(const PlaybackWorkerCore&) = delete;
  PlaybackWorkerCore(PlaybackWorkerCore&&) = delete;
  PlaybackWorkerCore& operator=(PlaybackWorkerCore&&) = delete;

  static Status Create(const PlaybackWorkerConfig& config,
                       const PlaybackWorkerEndpoints& endpoints,
                       PlaybackWorkerCore* output);

  // Cold path: prepares the sink once. A failure is terminal for this object.
  Status Initialize();

  PlaybackWorkerStepResult Step() noexcept;
  PlaybackWorkerState state() const noexcept {
    return terminal_fault_latched_ && state_ != PlaybackWorkerState::kStopped
               ? PlaybackWorkerState::kFaulted
               : state_;
  }

 private:
  enum class CancelPath : std::uint8_t {
    kNone = 0,
    kNoSink,
    kRendererOnly,
    kActiveSink,
  };

  enum class LifecyclePublishTransition : std::uint8_t {
    kNone = 0,
    kActive,
    kPreparedIdle,
    kAwaitingReferenceReset,
    kStopped,
  };

  PlaybackWorkerStepResult Result(PlaybackWorkerStepCode code) const noexcept;
  PlaybackWorkerStepResult ProcessUrgentCancel(
      const PlaybackCancelCommand& command) noexcept;
  PlaybackWorkerStepResult FlushPendingOutputs() noexcept;
  PlaybackWorkerStepResult ProcessLifecycleCommand(
      const PlaybackLifecycleCommand& command) noexcept;
  PlaybackWorkerStepResult ContinueArm() noexcept;
  PlaybackWorkerStepResult ContinueIngressQuiescence() noexcept;
  PlaybackWorkerStepResult ProcessActiveAudio() noexcept;
  PlaybackWorkerStepResult SubmitRenderedFrame() noexcept;
  PlaybackWorkerStepResult PumpCommitter() noexcept;
  PlaybackWorkerStepResult RenderIngressFrame() noexcept;
  PlaybackWorkerStepResult DrainRenderer() noexcept;

  void QueueLifecycleResult(const PlaybackLifecycleResult& result,
                            LifecyclePublishTransition transition =
                                LifecyclePublishTransition::kNone) noexcept;
  void QueueLocalCancelResult(
      const PlaybackLocalCancelCompletion& result) noexcept;
  bool QueueCriticalEvent(std::int32_t native_error) noexcept;
  bool QueueEndOfStreamEvent(
      const PlaybackCommitResult& commit_result) noexcept;
  bool AllocateEventSequence(std::uint64_t* sequence) noexcept;
  void RequireCancellation(std::int32_t native_error) noexcept;
  void ApplyLifecyclePublishTransition() noexcept;
  void ClearPipeline() noexcept;
  void ClearCancellationContext() noexcept;
  void EnterTerminalFault() noexcept;

  static bool GenerationsEqual(const PlaybackGeneration& lhs,
                               const PlaybackGeneration& rhs) noexcept;
  static bool IsCompletelyIdle(PlaybackWorkerState state) noexcept;

  PlaybackWorkerConfig config_{};
  PlaybackWorkerEndpoints endpoints_{};
  PlaybackRenderer24To48 renderer_{};
  PlaybackCommitter committer_{};
  PlaybackPcmFrame48k rendered_frame_{};

  PlaybackGeneration active_generation_{};
  PlaybackLifecycleCommand arm_command_{};
  PlaybackLifecycleCommand quiesce_command_{};
  PlaybackLifecycleCommand last_acknowledged_quiesce_command_{};
  PlaybackLifecycleCommand pending_reference_reset_command_{};
  PlaybackLifecycleCommand last_confirmed_reference_reset_command_{};
  PlaybackCancelCommand cancellation_command_{};
  std::uint64_t cancelled_pcm_incarnation_{0U};
  std::uint64_t accepted_generation_sample_frames_{0U};
  std::uint64_t last_accepted_chunk_sequence_{0U};
  std::uint64_t next_stream_event_sequence_{1U};
  // Generations are actor-unique and advance by epoch after cancellation.
  // This floor prevents a stale, non-cached cancel from fabricating a
  // never-armed proof for an identity the worker has already observed.
  std::uint32_t highest_seen_generation_epoch_{0U};
  std::uint32_t quiesce_discarded_frames_{0U};
  std::uint32_t quiesce_discarded_source_samples_{0U};

  PlaybackLifecycleResult pending_lifecycle_result_{};
  PlaybackLifecycleResult last_acknowledged_quiesce_result_{};
  PlaybackLocalCancelCompletion pending_local_cancel_result_{};
  PlaybackLocalCancelCompletion cached_local_cancel_result_{};
  PlaybackEndOfStreamAcceptedEvent pending_eos_event_{};
  PlaybackCriticalStreamEvent pending_critical_event_{};

  PlaybackWorkerState state_{PlaybackWorkerState::kUnconfigured};
  CancelPath cancel_path_{CancelPath::kNone};
  LifecyclePublishTransition lifecycle_transition_{
      LifecyclePublishTransition::kNone};
  bool renderer_armed_{false};
  bool committer_armed_{false};
  bool rendered_ready_{false};
  bool committer_chunk_pending_{false};
  bool renderer_drain_pending_{false};
  bool renderer_only_cancel_ready_{false};
  bool quiesce_in_progress_{false};
  bool pending_lifecycle_result_valid_{false};
  bool last_acknowledged_quiesce_valid_{false};
  bool pending_local_cancel_result_valid_{false};
  bool cached_local_cancel_result_valid_{false};
  bool pending_eos_event_valid_{false};
  bool pending_critical_event_valid_{false};
  bool event_sequence_exhausted_{false};
  bool fault_after_local_cancel_publish_{false};
  bool terminal_fault_latched_{false};
  bool last_confirmed_reference_reset_valid_{false};
};

}  // namespace boompi::audio

#endif  // BOOMPI_AUDIO_PLAYBACK_WORKER_H_
