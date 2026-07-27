#ifndef BOOMPI_AUDIO_PLAYBACK_RENDERER_24_TO_48_H_
#define BOOMPI_AUDIO_PLAYBACK_RENDERER_24_TO_48_H_

#include <cstdint>

#include "boompi/audio/playback_frame.h"
#include "boompi/audio/playback_gain_limiter.h"
#include "boompi/audio/playback_generation.h"
#include "boompi/audio/playback_resampler_24_to_48.h"
#include "boompi/event/status.h"

namespace boompi::audio {

enum class PlaybackRenderCode : std::uint8_t {
  kProduced = 0,
  kNotConfigured,
  kInvalidOutput,
  kInvalidGeneration,
  kNotArmed,
  kDrainPending,
  kNoDrainPending,
  kEpochMismatch,
  kStreamMismatch,
  kTurnMismatch,
  kInvalidInputFrame,
  kDiscontinuity,
  kSequenceBreak,
  kTimestampNotIncreasing,
  kStageFault,
  kFaulted,
};

struct PlaybackRenderResult final {
  PlaybackRenderCode code;
  std::uint32_t over_full_scale_samples;
  std::uint32_t limited_samples;
  std::uint32_t safety_saturated_samples;
  // True exactly when an EOS FIR tail remains pending after this call,
  // including rejected/null/stale calls that leave state unchanged.
  bool drain_required;

  constexpr bool produced() const noexcept {
    return code == PlaybackRenderCode::kProduced;
  }

  constexpr bool requires_new_generation() const noexcept {
    return code == PlaybackRenderCode::kInvalidInputFrame ||
           code == PlaybackRenderCode::kDiscontinuity ||
           code == PlaybackRenderCode::kSequenceBreak ||
           code == PlaybackRenderCode::kTimestampNotIncreasing ||
           code == PlaybackRenderCode::kStageFault ||
           code == PlaybackRenderCode::kFaulted;
  }
};

// Exclusive playback-worker facade with one fixed hot-path order:
// 24 kHz S16 mono -> 48 kHz wide S32 PCM -> volume/speaker gain -> duck ->
// block-peak limiter -> final S16 PCM. Scratch storage is a member, so Process
// and Drain perform
// no allocation, locking, I/O, logging, or floating-point work. Produced PCM
// is software output only; it is not proof of ALSA acceptance, audible
// playback, or AEC-reference publication. Disarm explicitly cancels a pending
// FIR tail without producing or claiming it.
//
// This facade validates the complete active generation but deliberately does
// not observe PlaybackEpochFence and is neither a cancellation barrier nor an
// Arm acknowledgement. The playback worker must apply its epoch fence/gate
// before invoking this stage. The actor owns session-wide generation
// uniqueness; this class defensively rejects its most recent epoch/stream.
class PlaybackRenderer24To48 final {
 public:
  PlaybackRenderer24To48() noexcept = default;
  PlaybackRenderer24To48(const PlaybackRenderer24To48&) = delete;
  PlaybackRenderer24To48& operator=(const PlaybackRenderer24To48&) = delete;
  PlaybackRenderer24To48(PlaybackRenderer24To48&&) = delete;
  PlaybackRenderer24To48& operator=(PlaybackRenderer24To48&&) = delete;

  static Status Create(const PlaybackGainLimiterConfig& gain_config,
                       PlaybackRenderer24To48* output);

  // A direct active-generation switch is rejected. If coordination of the
  // two private stages ever fails, both are disarmed and an internal fault is
  // latched; only a different generation can recover it.
  Status Arm(const PlaybackGeneration& generation);
  void Disarm() noexcept;
  Status SetDucked(bool ducked);

  // On failure, a non-null output has an invalid header and PCM storage is
  // left untouched. Null output, invalid generation, and stale generation
  // calls never advance FIR, continuity, ramp, or limiter state. For an EOS
  // input, Process produces only the declared exact-2N prefix with EOS false
  // and reports drain_required. Process and Arm are then rejected until Drain
  // emits the bounded FIR tail with EOS true and retires all three stages.
  PlaybackRenderResult Process(const TtsPcmFrame24k& input,
                               PlaybackPcmFrame48k* output) noexcept;
  PlaybackRenderResult Drain(PlaybackPcmFrame48k* output) noexcept;

 private:
  void LatchStageFault() noexcept;
  void RetireSuccessfulDrain() noexcept;

  PlaybackResampler24To48 resampler_{};
  PlaybackGainLimiter gain_limiter_{};
  ResampledPcmFrame48k resampled_scratch_{};
  PlaybackPcmFrame48k rendered_scratch_{};
  PlaybackGeneration active_generation_{};
  PlaybackGeneration last_generation_{};
  AudioFrameMetadata drain_metadata_{};
  std::uint16_t drain_source_offset_sample_frames_{0U};
  bool configured_{false};
  bool armed_{false};
  bool drain_pending_{false};
  bool faulted_{false};
};

}  // namespace boompi::audio

#endif  // BOOMPI_AUDIO_PLAYBACK_RENDERER_24_TO_48_H_
