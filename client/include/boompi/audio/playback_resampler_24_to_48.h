#ifndef BOOMPI_AUDIO_PLAYBACK_RESAMPLER_24_TO_48_H_
#define BOOMPI_AUDIO_PLAYBACK_RESAMPLER_24_TO_48_H_

#include <array>
#include <cstddef>
#include <cstdint>

#include "boompi/audio/frame_continuity_gate.h"
#include "boompi/audio/playback_frame.h"
#include "boompi/audio/playback_generation.h"
#include "boompi/event/status.h"

namespace boompi::audio {

enum class PlaybackResampleCode : std::uint8_t {
  kProduced = 0,
  kInvalidArgument,
  kNotArmed,
  kInvalidGeneration,
  kEpochMismatch,
  kStreamMismatch,
  kTurnMismatch,
  kInvalidInputFrame,
  kDiscontinuity,
  kSequenceBreak,
  kTimestampNotIncreasing,
  kDrainPending,
  kNoDrainPending,
  kFaulted,
};

struct PlaybackResampleResult final {
  PlaybackResampleCode code{PlaybackResampleCode::kInvalidArgument};
  // True exactly when an EOS convolution tail remains pending after this
  // call, including rejected/null/stale calls that leave state unchanged.
  bool drain_required{false};

  constexpr bool produced() const noexcept {
    return code == PlaybackResampleCode::kProduced;
  }
};

// Fixed 2x, linear-phase half-band interpolator for one 24 kHz mono TTS
// stream. One playback worker exclusively owns an instance; every method is
// serialized on that worker. Process is allocation-free and performs no
// locking, I/O, or floating-point work.
//
// Arm starts a new generation and resets all FIR/continuity state. A
// continuity or input-frame fault remains latched until Arm succeeds for a
// different epoch/stream identity. Disarm deliberately retains the retired
// identity through FrameContinuityGate so queued PCM cannot re-arm it. The
// application actor remains responsible for never reusing any generation
// identity during the complete session.
//
// EOS is a two-call transaction. Process emits the exact-2N prefix with EOS
// clear and drain_required set, then rejects further input. Drain emits the 63
// remaining full-convolution positions with EOS set and automatically disarms
// the generation. The 63rd position is an explicit terminal zero because the
// half-band endpoint h[64] is exactly zero; at most 62 tail positions are
// non-zero. A null or out-of-order Drain never advances state.
class PlaybackResampler24To48 final {
 public:
  static constexpr std::size_t kInterpolationFactor = 2U;
  static constexpr std::size_t kTapCount = 65U;
  static constexpr std::size_t kOddPhaseTapCount = 32U;
  static constexpr std::size_t kHistorySamples = 31U;
  static constexpr std::size_t kLatencyOutputSamples = 32U;
  static constexpr std::size_t kLatencyInputSamples =
      kLatencyOutputSamples / kInterpolationFactor;
  static constexpr std::size_t kDrainSamples =
      kTapCount - kInterpolationFactor;

  PlaybackResampler24To48() noexcept = default;

  Status Arm(const PlaybackGeneration& generation);
  void Disarm() noexcept;

  PlaybackResampleResult Process(const TtsPcmFrame24k& input,
                                 ResampledPcmFrame48k* output) noexcept;
  PlaybackResampleResult Drain(ResampledPcmFrame48k* output) noexcept;

 private:
  void ClearDspState() noexcept;
  std::int16_t SourceSample(const TtsPcmFrame24k& input,
                            std::size_t source_index,
                            std::size_t delay) const noexcept;
  std::int16_t DrainSourceSample(std::size_t zero_source_index,
                                 std::size_t delay) const noexcept;
  void UpdateHistory(const TtsPcmFrame24k& input) noexcept;

  FrameContinuityGate continuity_gate_{};
  PlaybackGeneration active_generation_{};
  std::array<std::int16_t, kHistorySamples> history_{};
  AudioFrameMetadata drain_metadata_{};
  std::uint16_t drain_source_offset_{0U};
  bool armed_{false};
  bool transform_faulted_{false};
  bool drain_pending_{false};
};

}  // namespace boompi::audio

#endif  // BOOMPI_AUDIO_PLAYBACK_RESAMPLER_24_TO_48_H_
