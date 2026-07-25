#ifndef BOOMPI_AUDIO_FRAME_CONTINUITY_GATE_H_
#define BOOMPI_AUDIO_FRAME_CONTINUITY_GATE_H_

#include <cstdint>

#include "boompi/audio/audio_format.h"
#include "boompi/event/status.h"

namespace boompi::audio {

enum class FrameContinuityResult : std::uint8_t {
  kAcceptedFirst = 0,
  kAcceptedContinuous,
  kNotArmed,
  kEpochMismatch,
  kStreamMismatch,
  kDiscontinuity,
  kSequenceBreak,
  kTimestampNotIncreasing,
  kFaulted,
};

// A gate is owned by exactly one PCM consumer. Every method, including Arm and
// Disarm, runs on that same consumer thread; an actor sends commands through a
// separate synchronized control path rather than calling this object directly.
// CheckAndAdvance is allocation-free and safe for the consumer's hot path.
class FrameContinuityGate final {
 public:
  Status Arm(std::uint32_t epoch, std::uint32_t stream_id);
  void Disarm() noexcept;

  FrameContinuityResult CheckAndAdvance(
      const AudioFrameMetadata& metadata) noexcept;

 private:
  void LatchFault() noexcept;

  std::uint32_t epoch_{0U};
  std::uint32_t stream_id_{0U};
  std::uint32_t last_sequence_{0U};
  std::uint64_t last_timestamp_us_{0U};
  bool armed_{false};
  bool has_previous_frame_{false};
  bool faulted_{false};
};

}  // namespace boompi::audio

#endif  // BOOMPI_AUDIO_FRAME_CONTINUITY_GATE_H_
