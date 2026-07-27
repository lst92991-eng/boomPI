#ifndef BOOMPI_AUDIO_PLAYBACK_GAIN_LIMITER_H_
#define BOOMPI_AUDIO_PLAYBACK_GAIN_LIMITER_H_

#include <cstdint>

#include "boompi/audio/playback_frame.h"
#include "boompi/audio/playback_generation.h"
#include "boompi/event/status.h"

namespace boompi::audio {

struct PlaybackGainLimiterConfig final {
  // Linear digital volume. The product default is 60; 100 is valid.
  std::uint8_t volume_percent{60U};
  // Additional linear digital speaker gain. Values above 100 can reduce
  // dynamic range through limiting and do not imply a hardware-safe volume.
  std::uint16_t speaker_gain_percent{100U};
  // Relative gain while ducked, measured against the normal configured gain.
  std::uint8_t duck_target_percent{25U};
  std::uint16_t duck_ramp_ms{80U};
  std::uint16_t recovery_ramp_ms{80U};
  // Symmetric output ceiling as a percentage of the S16 positive/negative
  // full-scale magnitudes. Zero is invalid; use volume zero for mute.
  std::uint8_t limiter_ceiling_percent{95U};
  // Peak attenuation attacks immediately and returns toward unity over this
  // release duration. Zero permits immediate release to each chunk's bound.
  std::uint16_t limiter_release_ms{80U};
};

enum class PlaybackGainLimiterCode : std::uint8_t {
  kProduced = 0,
  kNotConfigured,
  kNotArmed,
  kInvalidOutput,
  kInvalidInput,
  kEpochMismatch,
  kStreamMismatch,
  kTurnMismatch,
  kDiscontinuity,
  kFaulted,
};

struct PlaybackGainLimiterResult final {
  PlaybackGainLimiterCode code;
  // Counts pre-limiter scaled samples beyond the S16 numeric range. These
  // samples have not wrapped or been cast to S16 at this point.
  std::uint32_t over_full_scale_samples;
  // Counts samples whose actually applied limiter gain is below unity.
  std::uint32_t limited_samples;
  // Defensive final clamps after attenuation. Correctly configured integer
  // limiter arithmetic normally leaves this at zero.
  std::uint32_t safety_saturated_samples;

  constexpr bool produced() const noexcept {
    return code == PlaybackGainLimiterCode::kProduced;
  }

  constexpr bool requires_new_generation() const noexcept {
    return code == PlaybackGainLimiterCode::kDiscontinuity ||
           code == PlaybackGainLimiterCode::kFaulted;
  }
};

// One playback worker exclusively owns an instance and serializes Create,
// Arm, Disarm, SetDucked, and Process. Process performs no allocation,
// locking, I/O, logging, or floating-point work. This stage only transforms
// software PCM; produced output is not proof of ALSA acceptance, confirmed
// playback, or a published AEC reference.
class PlaybackGainLimiter final {
 public:
  static constexpr std::uint16_t kMaximumSpeakerGainPercent = 400U;
  static constexpr std::uint16_t kMaximumRampMs = 1000U;

  PlaybackGainLimiter() noexcept = default;
  PlaybackGainLimiter(const PlaybackGainLimiter&) = delete;
  PlaybackGainLimiter& operator=(const PlaybackGainLimiter&) = delete;
  PlaybackGainLimiter(PlaybackGainLimiter&&) = delete;
  PlaybackGainLimiter& operator=(PlaybackGainLimiter&&) = delete;

  // Cold-path conversion from human-readable percentages and milliseconds
  // to bounded Q16 gains and exact 48 kHz sample counts. A failed Create
  // leaves an existing instance unchanged.
  static Status Create(const PlaybackGainLimiterConfig& config,
                       PlaybackGainLimiter* output);

  // Arms a complete non-zero generation and clears duck ramp and fault
  // history. An active generation must first be disarmed. A successful EOS
  // frame is transformed and then automatically disarms this instance.
  // The actor must never reuse an earlier (epoch, stream_id) pair. This class
  // defensively rejects the most recently armed pair.
  Status Arm(const PlaybackGeneration& generation);
  void Disarm() noexcept;

  // Repeated requests for the current target are idempotent and do not
  // restart a ramp. Reversing direction begins at the current Q16 gain.
  Status SetDucked(bool ducked);

  // Consumes wide FIR output and performs the only S16 conversion in the
  // render chain. On every non-produced result a non-null output has an
  // invalid header, while its PCM storage remains untouched.
  PlaybackGainLimiterResult Process(
      const ResampledPcmFrame48k& input,
      PlaybackPcmFrame48k* output) noexcept;

 private:
  void ResetEnvelope() noexcept;
  void AdvanceEnvelopeOneSample() noexcept;
  void PrepareLimiterForChunk(std::uint32_t required_gain_q16) noexcept;
  void AdvanceLimiterOneSample(std::uint32_t required_gain_q16) noexcept;

  std::uint32_t base_gain_q16_{0U};
  std::uint32_t duck_target_q16_{0U};
  std::uint32_t current_envelope_q16_{0U};
  std::uint32_t ramp_start_q16_{0U};
  std::uint32_t ramp_target_q16_{0U};
  std::uint32_t ramp_total_samples_{0U};
  std::uint32_t ramp_elapsed_samples_{0U};
  std::uint32_t duck_ramp_samples_{0U};
  std::uint32_t recovery_ramp_samples_{0U};
  std::uint32_t limiter_gain_q16_{0U};
  std::uint32_t limiter_release_start_q16_{0U};
  std::uint32_t limiter_release_samples_{0U};
  std::uint32_t limiter_release_elapsed_samples_{0U};
  std::uint32_t positive_ceiling_{0U};
  std::uint32_t negative_ceiling_magnitude_{0U};
  PlaybackGeneration active_generation_{};
  PlaybackGeneration last_generation_{};
  bool configured_{false};
  bool armed_{false};
  bool duck_requested_{false};
  bool faulted_{false};
};

}  // namespace boompi::audio

#endif  // BOOMPI_AUDIO_PLAYBACK_GAIN_LIMITER_H_
