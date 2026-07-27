#ifndef BOOMPI_AUDIO_PLAYBACK_GENERATION_H_
#define BOOMPI_AUDIO_PLAYBACK_GENERATION_H_

#include <atomic>
#include <cstdint>

#include "boompi/event/status.h"

namespace boompi::audio {

struct PlaybackGeneration final {
  std::uint32_t epoch{0U};
  std::uint32_t turn_id{0U};
  std::uint32_t stream_id{0U};

  bool valid() const noexcept;
};

enum class PlaybackGenerationDecision : std::uint8_t {
  kAccepted = 0,
  kInvalidGeneration,
  kNotActive,
  kEpochMismatch,
  kStreamMismatch,
  kTurnMismatch,
};

enum class PlaybackGenerationRetireResult : std::uint8_t {
  kRetired = 0,
  kNotActive,
  kExpectedGenerationMismatch,
  kEpochUnchanged,
};

// This gate belongs exclusively to the playback thread. Activate requires the
// epoch snapshot returned by PlaybackEpochFence::Observe and never switches an
// active generation: the current generation must first be retired explicitly
// or invalidated after observing a different fence epoch.
//
// The gate remembers only the most recently retired (epoch, stream_id) pair
// and rejects its immediate reuse. The application actor remains responsible
// for never reusing any generation identity during the complete session.
class PlaybackGenerationGate final {
 public:
  Status Activate(const PlaybackGeneration& generation,
                  std::uint32_t observed_fence_epoch);

  PlaybackGenerationRetireResult Retire(
      const PlaybackGeneration& expected_generation) noexcept;
  PlaybackGenerationRetireResult InvalidateIfEpochChanged(
      std::uint32_t observed_epoch) noexcept;

  PlaybackGenerationDecision Check(
      const PlaybackGeneration& generation) const noexcept;

 private:
  void RememberRetiredGeneration() noexcept;

  PlaybackGeneration active_generation_{};
  std::uint32_t retired_epoch_{0U};
  std::uint32_t retired_stream_id_{0U};
  bool active_{false};
};

// The application actor is the only writer and the playback thread is the
// only reader. AdvanceTo publishes a strictly increasing non-zero epoch with
// release semantics; Observe reads it with acquire semantics. This fence is
// only a generation invalidation signal. It is not a cancellation
// acknowledgement, a wake-up primitive, or proof that queued/kernel PCM has
// already been discarded.
class PlaybackEpochFence final {
  static_assert(std::atomic<std::uint32_t>::is_always_lock_free,
                "playback requires lock-free 32-bit epoch atomics");

 public:
  PlaybackEpochFence() noexcept = default;
  PlaybackEpochFence(const PlaybackEpochFence&) = delete;
  PlaybackEpochFence& operator=(const PlaybackEpochFence&) = delete;
  PlaybackEpochFence(PlaybackEpochFence&&) = delete;
  PlaybackEpochFence& operator=(PlaybackEpochFence&&) = delete;

  Status AdvanceTo(std::uint32_t epoch);
  std::uint32_t Observe() const noexcept;

  // Cold-start compatibility check. There is no mutex-backed real-time
  // fallback when this returns false.
  bool is_lock_free() const noexcept;

 private:
  alignas(std::atomic<std::uint32_t>)
      std::atomic<std::uint32_t> epoch_{0U};
};

}  // namespace boompi::audio

#endif  // BOOMPI_AUDIO_PLAYBACK_GENERATION_H_
