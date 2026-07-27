#ifndef BOOMPI_AUDIO_PLAYBACK_CONTROL_H_
#define BOOMPI_AUDIO_PLAYBACK_CONTROL_H_

#include <cstdint>

#include "boompi/audio/playback_buffer.h"
#include "boompi/audio/playback_committer.h"
#include "boompi/audio/playback_generation.h"
#include "boompi/event/spsc_pod_queue.h"

namespace boompi::audio {

// Actor -> playback-worker normal-priority control. Cancel has a dedicated
// one-slot lane below so ordinary lifecycle backpressure can never delay the
// epoch fence/cancel path. The actor is the sole producer and the playback
// worker is the sole consumer of both lanes.
enum class PlaybackLifecycleCommandType : std::uint8_t {
  kUnset = 0,
  kArm,
  kSetDucked,
  kQuiesceIngress,
  kConfirmReferenceReset,
  kShutdown,
};

struct PlaybackLifecycleCommand final {
  PlaybackLifecycleCommandType type{PlaybackLifecycleCommandType::kUnset};
  std::uint64_t request_id{0U};
  PlaybackGeneration generation{};
  // Required by quiesce/confirmation after a real local cancel. Zero is
  // valid only for a no-sink path (never armed, or renderer-only Arm) that
  // proves the committer never owned this generation.
  std::uint64_t retired_pcm_incarnation{0U};
  bool value{false};

  bool valid() const noexcept;
};

struct PlaybackCancelCommand final {
  std::uint64_t request_id{0U};
  PlaybackGeneration generation{};
  // The actor publishes this command only after advancing PlaybackEpochFence
  // to this strictly newer epoch. The worker still observes the fence itself.
  std::uint32_t cancel_epoch{0U};

  bool valid() const noexcept;
};

enum class PlaybackArmResultCode : std::uint8_t {
  kUnset = 0,
  kAcknowledged,
  // A result can still be structurally valid at the cold fence value zero
  // when the echoed request/generation identity itself is complete.
  kInvalidCommand,
  // An observed value of zero is valid here: it can mean that no usable fence
  // epoch was available, and therefore still cannot authorize this generation.
  kFenceMismatch,
  kRendererRejected,
  kCommitterRejected,
  // Unlike a generic mismatch, this proves cancellation observed a concrete,
  // nonzero epoch different from the generation before Arm was acknowledged.
  kCancelledBeforeAcknowledgement,
  // A terminal initialization/core fault can precede the first nonzero fence.
  kFaulted,
};

struct PlaybackArmResult final {
  PlaybackArmResultCode code{PlaybackArmResultCode::kUnset};
  std::uint64_t request_id{0U};
  PlaybackGeneration generation{};
  std::uint32_t observed_fence_epoch{0U};

  bool acknowledged() const noexcept;
  bool valid() const noexcept;
};

enum class PlaybackDuckResultCode : std::uint8_t {
  kUnset = 0,
  kAcknowledged,
  kInvalidCommand,
  kNotArmed,
  kGenerationMismatch,
  kRendererRejected,
};

struct PlaybackDuckResult final {
  PlaybackDuckResultCode code{PlaybackDuckResultCode::kUnset};
  std::uint64_t request_id{0U};
  PlaybackGeneration generation{};
  bool ducked{false};

  bool acknowledged() const noexcept;
  bool valid() const noexcept;
};

enum class PlaybackIngressQuiesceCode : std::uint8_t {
  kUnset = 0,
  kAcknowledged,
  // One bounded pass reached its iteration limit. The worker retains the
  // transaction and continues later; this is not an ACK and not empty proof.
  kInProgress,
  kInvalidCommand,
  kProducerNotStopped,
  kGenerationMismatch,
  kIncarnationMismatch,
  kFaulted,
};

struct PlaybackIngressQuiescedAck final {
  PlaybackIngressQuiesceCode code{PlaybackIngressQuiesceCode::kUnset};
  std::uint64_t request_id{0U};
  PlaybackGeneration generation{};
  std::uint64_t retired_pcm_incarnation{0U};
  std::uint32_t discarded_frames{0U};
  std::uint32_t discarded_source_samples{0U};
  // These facts are both required for kAcknowledged. The actor may request
  // quiescence only after its producer-stop ACK; the worker proves a later
  // empty observation by completing a bounded pass below the queue capacity.
  bool producer_stop_precondition{false};
  bool empty_observed_after_producer_stop{false};
  bool reached_iteration_bound{false};

  bool acknowledged() const noexcept;
  bool valid() const noexcept;
};

enum class PlaybackShutdownResultCode : std::uint8_t {
  kUnset = 0,
  kAcknowledged,
  kInvalidCommand,
  kActiveGeneration,
  kFaulted,
};

struct PlaybackShutdownResult final {
  PlaybackShutdownResultCode code{PlaybackShutdownResultCode::kUnset};
  std::uint64_t request_id{0U};

  bool acknowledged() const noexcept;
  bool valid() const noexcept;
};

enum class PlaybackLifecycleResultType : std::uint8_t {
  kUnset = 0,
  kArm,
  kDuck,
  kIngressQuiesced,
  kReferenceReset,
  kShutdown,
};

// Discriminated POD result for the normal-priority lane. Only the member named
// by type is meaningful. Critical local-cancel completion has its own one-slot
// lane, so a full ordinary result ring cannot lose a Drop/Prepare ACK.
struct PlaybackLifecycleResult final {
  PlaybackLifecycleResultType type{PlaybackLifecycleResultType::kUnset};
  PlaybackArmResult arm{};
  PlaybackDuckResult duck{};
  PlaybackIngressQuiescedAck ingress_quiesced{};
  PlaybackReferenceResetResult reference_reset{};
  PlaybackShutdownResult shutdown{};

  bool valid() const noexcept;
};

enum class PlaybackLocalCancelCode : std::uint8_t {
  kUnset = 0,
  kAcknowledged,
  // No renderer/committer generation ever became active. This establishes a
  // local no-write barrier but does not fabricate Drop/Prepare facts.
  kAlreadyQuiescent,
  // Renderer Arm completed, but cancellation won the race before committer
  // Arm. The worker has disarmed renderer state and proves no sink write or
  // accepted-reference publication was possible for this generation.
  kRendererOnlyQuiesced,
  kInvalidCommand,
  kGenerationMismatch,
  kFenceNotAdvanced,
  kRejected,
  // Terminal typed completion. This can mean either that Drop/Prepare
  // completed but another identity space was exhausted, or that Drop retired
  // the old PCM timeline but the PCM incarnation itself could not advance.
  // It is never an ordinary cancellation-barrier ACK.
  kRestartRequired,
  kFaulted,
};

struct PlaybackLocalCancelCompletion final {
  PlaybackLocalCancelCode code{PlaybackLocalCancelCode::kUnset};
  std::uint64_t request_id{0U};
  PlaybackGeneration generation{};
  std::uint32_t observed_fence_epoch{0U};
  PlaybackCancelResult committer_result{};
  // This initial bounded drain is diagnostic only. It precedes the stable
  // producer-stop -> ingress-quiesce barrier and never proves lasting empty.
  PlaybackDrainResult initial_ingress_drain{};
  bool worker_holds_no_ingress_lease{false};
  bool producer_lease_may_publish_late{true};
  bool reference_reset_required{false};
  // No-sink proof fields. kAlreadyQuiescent proves this exact generation never
  // crossed either Arm boundary; kRendererOnlyQuiesced proves only renderer
  // crossed Arm. Active Drop/Prepare completion leaves these claims false even
  // when its accepted counters happen to be zero.
  bool renderer_generation_never_armed{false};
  // True only after PlaybackRenderer24To48::Disarm completed after observing
  // at least observed_fence_epoch. Required for every path that had renderer
  // state, including the renderer-only Arm/cancel race.
  bool renderer_disarmed_after_cancel_fence{false};
  bool committer_generation_never_armed{false};
  // Exact state proof for no-sink paths. A committer that ever Arm'ed the
  // generation must use the normal Drop/Prepare cancellation path instead.
  bool committer_prepared_idle{false};
  bool no_accepted_pcm_for_generation{false};

  bool acknowledged() const noexcept;
  bool already_quiescent() const noexcept;
  bool renderer_only_quiesced() const noexcept;
  bool terminal_restart_required() const noexcept;
  bool valid() const noexcept;
};

constexpr std::uint32_t kPlaybackLifecycleCommandCapacity = 8U;
constexpr std::uint32_t kPlaybackLifecycleResultCapacity = 16U;

using PlaybackUrgentCancelMailbox =
    event::SpscPodQueue<PlaybackCancelCommand, 1U>;
using PlaybackLifecycleCommandMailbox =
    event::SpscPodQueue<PlaybackLifecycleCommand,
                        kPlaybackLifecycleCommandCapacity>;
using PlaybackLocalCancelResultMailbox =
    event::SpscPodQueue<PlaybackLocalCancelCompletion, 1U>;
using PlaybackLifecycleResultMailbox =
    event::SpscPodQueue<PlaybackLifecycleResult,
                        kPlaybackLifecycleResultCapacity>;

// Actor -> network producer permits. Start is issued only after a matching
// playback Arm ACK. Stop is published in parallel with urgent local cancel,
// after the actor advances the playback fence. Separate one-slot lanes keep a
// stop request independent of start/control backpressure. The network worker
// MUST process in this order on every iteration and immediately before each
// data publish: Stop lane -> current fence/authorization check -> Start lane ->
// data publish. A popped Start permit is not durable authorization: before it
// enables publishing or acquires a write lease, the worker must recheck that no
// Stop is pending and that the exact generation is still actor-authorized.
// Duplicate Start for the same request/generation replays the cached
// kAcknowledged result; kAlreadyStarted means a conflicting active start and
// therefore never reports publish_enabled. Implementation remains a later
// network-worker phase.
struct TtsProducerStartPermit final {
  std::uint64_t request_id{0U};
  PlaybackGeneration generation{};

  bool valid() const noexcept;
};

struct TtsProducerStopRequest final {
  std::uint64_t request_id{0U};
  PlaybackGeneration generation{};
  std::uint32_t cancel_epoch{0U};

  bool valid() const noexcept;
};

enum class TtsProducerStartCode : std::uint8_t {
  kUnset = 0,
  kAcknowledged,
  kGenerationMismatch,
  kAlreadyStarted,
  kFailed,
};

struct TtsProducerStartAck final {
  TtsProducerStartCode code{TtsProducerStartCode::kUnset};
  std::uint64_t request_id{0U};
  PlaybackGeneration generation{};
  bool publish_enabled{false};
  // This success fact can remain true on kFailed when a clean producer proved
  // it owned no old lease before a later enable operation failed.
  bool owns_no_preexisting_write_lease{false};

  bool acknowledged() const noexcept;
  bool valid() const noexcept;
};

using TtsProducerStartPermitMailbox =
    event::SpscPodQueue<TtsProducerStartPermit, 1U>;
using TtsProducerStopRequestMailbox =
    event::SpscPodQueue<TtsProducerStopRequest, 1U>;
using TtsProducerStartAckMailbox = event::SpscPodQueue<TtsProducerStartAck, 1U>;

// Network TTS producer -> application actor. Acknowledged means the producer
// will no longer acquire or publish the old generation and owns no write
// lease. It does not mean the ingress queue has been consumed.
enum class TtsProducerStopCode : std::uint8_t {
  kUnset = 0,
  kAcknowledged,
  kInvalidRequest,
  kGenerationMismatch,
  kFailed,
};

struct TtsProducerStopAck final {
  TtsProducerStopCode code{TtsProducerStopCode::kUnset};
  std::uint64_t request_id{0U};
  PlaybackGeneration generation{};
  // Echoes the request payload. kInvalidRequest is reserved for an otherwise
  // valid request envelope whose cancel epoch is not newer than generation.
  std::uint32_t observed_cancel_epoch{0U};
  // Failure ACKs preserve monotonic partial facts: disabling new publishes can
  // succeed even if releasing an already-held lease later fails.
  bool producer_stopped{false};
  bool producer_write_lease_released{false};

  bool acknowledged() const noexcept;
  bool valid() const noexcept;
};

using TtsProducerStopAckMailbox = event::SpscPodQueue<TtsProducerStopAck, 1U>;

// Actor -> DSP/reference consumer. The request is emitted only after local
// cancel supplies the exact retired incarnation. Sequence/sample watermarks
// are diagnostics, not a demand that every sequence exist in the ledger.
struct DspReferenceResetRequest final {
  std::uint64_t request_id{0U};
  PlaybackGeneration generation{};
  std::uint64_t retired_pcm_incarnation{0U};
  std::uint64_t last_accepted_chunk_sequence{0U};
  std::uint64_t accepted_generation_sample_frames{0U};

  bool valid() const noexcept;
};

// DSP/reference consumer -> application actor. The reset is identified by the
// retired PCM incarnation, never by an accepted sequence watermark: a
// malformed positive sink result can consume a sequence without publishing a
// ledger item. Acknowledged proves only the listed digital/DSP facts.
enum class DspReferenceResetCode : std::uint8_t {
  kUnset = 0,
  kAcknowledged,
  kInvalidRequest,
  kGenerationMismatch,
  kIncarnationMismatch,
  kBackendFailed,
  kUnavailable,
};

struct DspReferenceResetAck final {
  DspReferenceResetCode code{DspReferenceResetCode::kUnset};
  std::uint64_t request_id{0U};
  PlaybackGeneration generation{};
  std::uint64_t retired_pcm_incarnation{0U};
  // kBackendFailed preserves any completed prefix of these irreversible facts;
  // only kAcknowledged proves the entire barrier.
  bool accepted_consumer_lease_released{false};
  bool retired_incarnation_discarded{false};
  // Observed only after local cancel closed the old accepted-chunk producer;
  // releasing one held item is not sufficient to assert this queue barrier.
  bool accepted_queue_empty_after_local_cancel{false};
  bool dsp_history_reset{false};

  bool acknowledged() const noexcept;
  bool valid() const noexcept;
};

using DspReferenceResetRequestMailbox =
    event::SpscPodQueue<DspReferenceResetRequest, 1U>;
using DspReferenceResetAckMailbox =
    event::SpscPodQueue<DspReferenceResetAck, 1U>;

// Playback worker -> actor asynchronous stream facts. These are not command
// replies and do not use EventBus from the real-time worker. EOS accepted is
// still not presented/played/audible or permission to enter FOLLOW_UP.
struct PlaybackEndOfStreamAcceptedEvent final {
  std::uint64_t event_sequence{0U};
  PlaybackGeneration generation{};
  std::uint64_t pcm_incarnation{0U};
  std::uint64_t accepted_generation_sample_frames{0U};
  std::uint64_t last_accepted_chunk_sequence{0U};

  bool valid() const noexcept;
};

enum class PlaybackCriticalStreamEventCode : std::uint8_t {
  kUnset = 0,
  kCancellationRequired,
  kFaulted,
};

struct PlaybackCriticalStreamEvent final {
  PlaybackCriticalStreamEventCode code{PlaybackCriticalStreamEventCode::kUnset};
  std::uint64_t event_sequence{0U};
  PlaybackGeneration generation{};
  std::uint64_t pcm_incarnation{0U};
  std::uint64_t accepted_generation_sample_frames{0U};
  std::uint64_t last_accepted_chunk_sequence{0U};
  std::int32_t native_error{0};

  bool valid() const noexcept;
};

constexpr std::uint32_t kPlaybackEndOfStreamEventCapacity = 16U;
using PlaybackEndOfStreamEventMailbox =
    event::SpscPodQueue<PlaybackEndOfStreamAcceptedEvent,
                        kPlaybackEndOfStreamEventCapacity>;

// EOS is a terminal accepted-stream fact even though it is not presentation
// proof. If TryPush returns false, the worker must retain and retry the exact
// event before accepting another generation; it must not silently discard it.

// Cancellation-required and fault events have an independent one-slot lane,
// so ordinary EOS backpressure cannot hide them. Once a valid critical event
// exists, the sole worker producer must stop acquiring ingress, rendering, and
// committing. If TryPush returns false, it must retain the exact event in local
// pending state and retry it before all non-urgent work; it must never
// overwrite, downgrade, or silently drop the event. Urgent cancel is the sole
// exception: it must remain serviceable while a critical event is pending so
// teardown can always make progress. The worker core enforces this retry and
// urgent-cancel invariant.
// PlaybackWorkerCore currently emits kCancellationRequired here. kFaulted is
// reserved for a future runner/platform adapter that can attach a truthful
// negative native error; pure core terminal faults use sticky Step/state.
using PlaybackCriticalStreamEventMailbox =
    event::SpscPodQueue<PlaybackCriticalStreamEvent, 1U>;

enum class PlaybackCancellationJoinState : std::uint8_t {
  kIdle = 0,
  kAwaitingBarriers,
  kReadyForPlaybackConfirmation,
  kCompleted,
  kFaulted,
};

enum class PlaybackCancellationJoinCode : std::uint8_t {
  kUnset = 0,
  kStarted,
  kProgress,
  kDuplicate,
  kReadyForPlaybackConfirmation,
  kCompleted,
  kNotActive,
  kInvalidInput,
  kRequestMismatch,
  kRequestNotIncreasing,
  kGenerationMismatch,
  kEpochNotIncreasing,
  kFenceEpochMismatch,
  kIncarnationMismatch,
  kConflictingDuplicate,
  kOutOfOrder,
  kBarrierFailed,
  kTerminalRestartRequired,
};

struct PlaybackCancellationJoinResult final {
  PlaybackCancellationJoinCode code{PlaybackCancellationJoinCode::kUnset};
  PlaybackCancellationJoinState state{PlaybackCancellationJoinState::kIdle};
  std::uint64_t request_id{0U};
  PlaybackGeneration generation{};
  std::uint32_t cancel_epoch{0U};
  std::uint64_t retired_pcm_incarnation{0U};
  bool producer_stopped{false};
  bool playback_local_cancelled{false};
  bool ingress_quiesced{false};
  bool dsp_reference_reset{false};
  bool playback_reference_reset_confirmed{false};
  bool ready_for_playback_confirmation{false};
  bool can_arm_next_generation{false};
};

// Application-actor-owned exact cancellation join. Begin follows a successful
// epoch-fence advance. A real active cancellation requires producer stop,
// local Drop/Prepare, a later stable ingress-empty ACK, DSP reset by retired
// incarnation, and finally worker-side ConfirmReferenceReset. No intermediate
// result proves DAC/speaker silence or authorizes a new playback generation.
class PlaybackCancellationJoin final {
 public:
  PlaybackCancellationJoin() noexcept = default;
  PlaybackCancellationJoin(const PlaybackCancellationJoin&) = delete;
  PlaybackCancellationJoin& operator=(const PlaybackCancellationJoin&) = delete;

  PlaybackCancellationJoinResult Begin(
      const PlaybackCancelCommand& command) noexcept;
  PlaybackCancellationJoinResult ObserveProducerStop(
      const TtsProducerStopAck& ack) noexcept;
  PlaybackCancellationJoinResult ObserveLocalCancel(
      const PlaybackLocalCancelCompletion& completion) noexcept;
  PlaybackCancellationJoinResult ObserveIngressQuiesced(
      const PlaybackIngressQuiescedAck& ack) noexcept;
  PlaybackCancellationJoinResult ObserveDspReferenceReset(
      const DspReferenceResetAck& ack) noexcept;
  PlaybackCancellationJoinResult ObservePlaybackReferenceReset(
      const PlaybackReferenceResetResult& result) noexcept;

  PlaybackCancellationJoinState state() const noexcept { return state_; }
  bool can_arm_next_generation() const noexcept {
    return state_ == PlaybackCancellationJoinState::kCompleted;
  }

 private:
  PlaybackCancellationJoinResult Snapshot(
      PlaybackCancellationJoinCode code) const noexcept;
  PlaybackCancellationJoinResult EvaluateProgress() noexcept;
  bool Matches(std::uint64_t request_id,
               const PlaybackGeneration& generation) const noexcept;

  PlaybackCancellationJoinState state_{PlaybackCancellationJoinState::kIdle};
  std::uint64_t request_id_{0U};
  PlaybackGeneration generation_{};
  std::uint32_t cancel_epoch_{0U};
  std::uint64_t retired_pcm_incarnation_{0U};
  bool producer_stopped_{false};
  bool playback_local_cancelled_{false};
  bool ingress_quiesced_{false};
  bool dsp_reference_reset_{false};
  bool playback_reference_reset_confirmed_{false};
  bool reference_reset_required_{false};
  bool local_no_reference_path_{false};
  TtsProducerStopAck producer_stop_ack_{};
  PlaybackLocalCancelCompletion local_cancel_completion_{};
  PlaybackIngressQuiescedAck ingress_quiesced_ack_{};
  DspReferenceResetAck dsp_reference_reset_ack_{};
  PlaybackReferenceResetResult playback_reference_reset_result_{};
};

}  // namespace boompi::audio

#endif  // BOOMPI_AUDIO_PLAYBACK_CONTROL_H_
