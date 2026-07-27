#ifndef BOOMPI_AUDIO_VAD_UTTERANCE_CONTROLLER_H_
#define BOOMPI_AUDIO_VAD_UTTERANCE_CONTROLLER_H_

#include <array>
#include <cstddef>
#include <cstdint>

#include "boompi/audio/audio_frame.h"
#include "boompi/audio/frame_continuity_gate.h"
#include "boompi/event/status.h"

namespace boompi::audio {

constexpr std::uint32_t kVadFrameDurationMs = 20U;
constexpr std::size_t kVadPreRollFrameCount = 25U;
constexpr std::size_t kVadUplinkQueueCapacity = 40U;
constexpr std::uint32_t kVadFirstSpeechTimeoutMs = 6000U;
constexpr std::uint32_t kVadTrailingSilenceFrameCount = 35U;
constexpr std::uint32_t kVadMaximumUtteranceFrameCount = 3000U;
constexpr std::uint32_t kVadDuckCandidateFrameCount = 4U;
constexpr std::uint32_t kVadBargeInConfirmFrameCount = 8U;

static_assert(kVadPreRollFrameCount * kVadFrameDurationMs == 500U,
              "VAD pre-roll must remain exactly 500 ms");
static_assert(kVadUplinkQueueCapacity * kVadFrameDurationMs == 800U,
              "VAD uplink queue must remain bounded to 800 ms");
static_assert(kVadTrailingSilenceFrameCount * kVadFrameDurationMs == 700U,
              "VAD trailing silence must remain exactly 700 ms");
static_assert(kVadMaximumUtteranceFrameCount * kVadFrameDurationMs == 60000U,
              "VAD utterance limit must remain exactly 60 seconds");
static_assert(kVadDuckCandidateFrameCount * kVadFrameDurationMs == 80U,
              "VAD duck candidate must remain exactly 80 ms");
static_assert(kVadBargeInConfirmFrameCount * kVadFrameDurationMs == 160U,
              "VAD barge-in confirmation must remain exactly 160 ms");

// A real VAD implementation supplies one observation for each AEC-processed
// 16 kHz mono frame. This enum is deliberately not an energy detector and
// Snowboy's diagnostic silence result must not be translated into it.
enum class VadObservation : std::uint8_t {
  kUnset = 0,
  kSilence,
  kSpeech,
};

enum class VadControllerCode : std::uint8_t {
  kUnset = 0,
  kAccepted,
  kStaleFrameDropped,
  kListeningTimedOut,
  kUtteranceStarted,
  kUtteranceEnded,
  kCancelled,
  kFaulted,
};

enum class VadUtteranceEndReason : std::uint8_t {
  kNone = 0,
  kTrailingSilence,
  kMaximumDuration,
  kFirstSpeechTimeout,
  kCancelled,
  kInvalidInput,
  kContinuityFault,
  kPreRollUnavailable,
  kOutputOverflow,
};

enum class VadControlIntent : std::uint16_t {
  kNone = 0U,
  kDuckPlayback = 1U << 0U,
  kRestorePlayback = 1U << 1U,
  kInterruptConfirmed = 1U << 2U,
  kCancelResponse = 1U << 3U,
  kClearPlaybackQueue = 1U << 4U,
  kStartUtterance = 1U << 5U,
  kEndUtterance = 1U << 6U,
  kCancelUserTurn = 1U << 7U,
};

using VadControlIntentMask = std::uint16_t;

constexpr VadControlIntentMask ToMask(const VadControlIntent intent) noexcept {
  return static_cast<VadControlIntentMask>(intent);
}

constexpr VadControlIntentMask operator|(const VadControlIntent lhs,
                                         const VadControlIntent rhs) noexcept {
  return static_cast<VadControlIntentMask>(ToMask(lhs) | ToMask(rhs));
}

constexpr bool HasVadControlIntent(const VadControlIntentMask mask,
                                   const VadControlIntent intent) noexcept {
  return (mask & ToMask(intent)) != 0U;
}

struct VadControllerResult final {
  VadControllerCode code;
  VadUtteranceEndReason end_reason;
  VadControlIntentMask intents;
  std::uint32_t epoch;
  std::uint32_t user_turn_id;
  std::uint32_t response_turn_id;
  std::uint32_t queued_uplink_frames;

  constexpr bool valid() const noexcept {
    return code != VadControllerCode::kUnset;
  }
};

// VadUtteranceController is a synchronous orchestration core owned by exactly
// one wake/VAD worker. Arm, control commands, Process and TryPopUplinkFrame all
// run on that owner thread. It creates no thread and performs no allocation,
// sleep, logging, file, device or network I/O. The worker drains the bounded
// output into its own SPSC uplink producer; failure to drain is an overflow and
// cancels the generation rather than buffering stale speech.
class VadUtteranceController final {
 public:
  VadUtteranceController() noexcept = default;
  VadUtteranceController(const VadUtteranceController&) = delete;
  VadUtteranceController& operator=(const VadUtteranceController&) = delete;

  Status Arm(std::uint32_t epoch, std::uint32_t stream_id);
  void Disarm() noexcept;

  // Called after a wake/follow-up command has allocated the next user turn.
  // Existing pre-roll is intentionally preserved across this command.
  Status BeginListening(std::uint32_t user_turn_id,
                        std::uint64_t command_timestamp_us);

  // Starts near-end speech monitoring while one assistant response is playing.
  // The actor allocates next_user_turn_id before issuing this command.
  Status BeginPlaybackBargeIn(std::uint32_t response_turn_id,
                              std::uint32_t next_user_turn_id);
  VadControllerResult EndPlayback() noexcept;

  // Explicit cancellation flushes pre-roll and pending uplink PCM and retires
  // the generation. A subsequent Arm must use a strictly newer epoch; epoch
  // exhaustion requires reconstructing the controller under a stopped worker.
  VadControllerResult Cancel() noexcept;

  VadControllerResult Process(const Mono16kFrame& frame,
                              VadObservation observation) noexcept;

  // Pop is destructive. The worker must reserve downstream SPSC capacity
  // before calling so a successful pop cannot be lost on publish.
  bool TryPopUplinkFrame(Mono16kFrame* output) noexcept;
  std::uint32_t queued_uplink_frames() const noexcept;
  bool armed() const noexcept;
  bool faulted() const noexcept;

 private:
  enum class CapturePhase : std::uint8_t {
    kMonitoring = 0,
    kAwaitingFirstSpeech,
    kStreamingUtterance,
  };

  static constexpr std::uint64_t kFirstSpeechTimeoutUs =
      static_cast<std::uint64_t>(kVadFirstSpeechTimeoutMs) * 1000U;

  VadControllerResult MakeResult(VadControllerCode code) const noexcept;
  VadControllerResult LatchFault(VadUtteranceEndReason reason) noexcept;
  VadControllerResult FinishUtterance(
      VadUtteranceEndReason reason) noexcept;
  void FlushAndRetireGeneration() noexcept;
  void FlushBuffersAndState() noexcept;
  void ResetPlaybackState() noexcept;
  void PushPreRoll(const Mono16kFrame& frame) noexcept;
  bool EnqueueFrame(const Mono16kFrame& frame,
                    std::uint32_t turn_id) noexcept;
  bool EnqueuePreRoll(std::uint32_t turn_id) noexcept;
  VadControllerResult StartUtterance(std::uint32_t initial_speech_frames,
                                     bool barge_in) noexcept;
  VadControllerResult ProcessPlaybackObservation(
      VadObservation observation) noexcept;
  VadControlIntentMask ActiveCancellationIntents() const noexcept;

  FrameContinuityGate continuity_gate_{};
  std::array<Mono16kFrame, kVadPreRollFrameCount> pre_roll_{};
  std::array<Mono16kFrame, kVadUplinkQueueCapacity> uplink_queue_{};

  std::size_t pre_roll_write_index_{0U};
  std::size_t pre_roll_count_{0U};
  std::size_t uplink_read_index_{0U};
  std::size_t uplink_write_index_{0U};
  std::size_t uplink_count_{0U};

  std::uint32_t epoch_{0U};
  std::uint32_t highest_epoch_{0U};
  std::uint32_t stream_id_{0U};
  std::uint32_t user_turn_id_{0U};
  std::uint32_t response_turn_id_{0U};
  std::uint32_t utterance_frame_count_{0U};
  std::uint32_t trailing_silence_frames_{0U};
  std::uint32_t barge_in_speech_frames_{0U};
  std::uint64_t first_speech_deadline_us_{0U};
  CapturePhase capture_phase_{CapturePhase::kMonitoring};
  bool armed_{false};
  bool faulted_{false};
  bool playback_active_{false};
  bool duck_requested_{false};
};

}  // namespace boompi::audio

#endif  // BOOMPI_AUDIO_VAD_UTTERANCE_CONTROLLER_H_
