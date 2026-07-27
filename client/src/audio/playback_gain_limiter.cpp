#include "boompi/audio/playback_gain_limiter.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>

namespace boompi::audio {
namespace {

constexpr std::uint32_t kQ16One = 65536U;
constexpr std::uint32_t kPercentScale = 100U;
constexpr std::uint32_t kSamplesPerMillisecond =
    ResampledPcmFrame48k::kSampleRateHz / 1000U;
constexpr std::uint32_t kMaximumGainQ16 =
    (PlaybackGainLimiter::kMaximumSpeakerGainPercent * kQ16One) /
    kPercentScale;
constexpr std::uint64_t kMaximumCombinedGainProduct =
    static_cast<std::uint64_t>(kMaximumGainQ16) * kQ16One;
constexpr std::int64_t kMaximumInputMagnitude =
    -static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::min());
constexpr std::int64_t kMaximumScaledSampleProduct =
    kMaximumInputMagnitude * kMaximumGainQ16;
constexpr std::int64_t kMaximumPreLimiterMagnitude =
    (kMaximumScaledSampleProduct + (kQ16One / 2U)) / kQ16One;
constexpr std::int64_t kMaximumLimiterProduct =
    kMaximumPreLimiterMagnitude * kQ16One;

static_assert(std::is_trivial<PlaybackGainLimiterResult>::value,
              "playback gain result must remain a POD value");
static_assert(std::is_standard_layout<PlaybackGainLimiterResult>::value,
              "playback gain result must remain standard-layout");
static_assert(ResampledPcmFrame48k::kSampleRateHz % 1000U == 0U,
              "ramp millisecond conversion must be exact");
static_assert(kMaximumCombinedGainProduct <=
                  std::numeric_limits<std::uint64_t>::max(),
              "combined Q16 gain multiplication must not overflow");
static_assert(kMaximumScaledSampleProduct <
                  std::numeric_limits<std::int64_t>::max(),
              "scaled PCM multiplication must not overflow");
static_assert(kMaximumLimiterProduct <
                  std::numeric_limits<std::int64_t>::max(),
              "block limiter multiplication must not overflow");

constexpr PlaybackGainLimiterResult MakeResult(
    const PlaybackGainLimiterCode code,
    const std::uint32_t over_full_scale_samples = 0U,
    const std::uint32_t limited_samples = 0U,
    const std::uint32_t safety_saturated_samples = 0U) noexcept {
  return {code, over_full_scale_samples, limited_samples,
          safety_saturated_samples};
}

std::uint32_t PercentToQ16(const std::uint32_t percent) noexcept {
  const auto numerator =
      static_cast<std::uint64_t>(percent) * kQ16One +
      (kPercentScale / 2U);
  return static_cast<std::uint32_t>(numerator / kPercentScale);
}

std::uint32_t MultiplyQ16Bounded(const std::uint32_t lhs,
                                 const std::uint32_t rhs) noexcept {
  const auto product = static_cast<std::uint64_t>(lhs) * rhs;
  const auto rounded = (product + (kQ16One / 2U)) / kQ16One;
  return rounded > kMaximumGainQ16
             ? kMaximumGainQ16
             : static_cast<std::uint32_t>(rounded);
}

std::int64_t ScaleSampleQ16(const std::int32_t sample,
                            const std::uint32_t gain_q16) noexcept {
  const auto bounded_gain =
      gain_q16 > kMaximumGainQ16 ? kMaximumGainQ16 : gain_q16;
  const auto product = static_cast<std::int64_t>(sample) * bounded_gain;
  constexpr std::int64_t kHalf = static_cast<std::int64_t>(kQ16One / 2U);
  if (product >= 0) {
    return (product + kHalf) / kQ16One;
  }
  return -((-product + kHalf) / kQ16One);
}

std::int64_t ScaleSignedValueQ16(const std::int64_t value,
                                 const std::uint32_t gain_q16) noexcept {
  const std::uint32_t bounded_gain =
      gain_q16 > kQ16One ? kQ16One : gain_q16;
  const std::int64_t product = value * bounded_gain;
  constexpr std::int64_t kHalf = static_cast<std::int64_t>(kQ16One / 2U);
  if (product >= 0) {
    return (product + kHalf) / kQ16One;
  }
  return -((-product + kHalf) / kQ16One);
}

void AdvanceEnvelopeState(const std::uint32_t ramp_start_q16,
                          const std::uint32_t ramp_target_q16,
                          std::uint32_t* const current_envelope_q16,
                          std::uint32_t* const ramp_total_samples,
                          std::uint32_t* const ramp_elapsed_samples) noexcept {
  if (*ramp_total_samples == 0U ||
      *ramp_elapsed_samples >= *ramp_total_samples) {
    return;
  }

  ++(*ramp_elapsed_samples);
  const bool increasing = ramp_target_q16 >= ramp_start_q16;
  const std::uint32_t distance =
      increasing ? ramp_target_q16 - ramp_start_q16
                 : ramp_start_q16 - ramp_target_q16;
  const auto numerator =
      static_cast<std::uint64_t>(distance) * (*ramp_elapsed_samples) +
      (*ramp_total_samples / 2U);
  const auto traversed =
      static_cast<std::uint32_t>(numerator / *ramp_total_samples);
  *current_envelope_q16 =
      increasing ? ramp_start_q16 + traversed
                 : ramp_start_q16 - traversed;
  if (*ramp_elapsed_samples == *ramp_total_samples) {
    *current_envelope_q16 = ramp_target_q16;
    *ramp_total_samples = 0U;
  }
}

std::uint32_t ComputeBlockAttenuationQ16(
    const std::uint64_t maximum_positive,
    const std::uint64_t maximum_negative_magnitude,
    const std::uint32_t positive_ceiling,
    const std::uint32_t negative_ceiling_magnitude) noexcept {
  std::uint64_t attenuation_q16 = kQ16One;
  if (maximum_positive > positive_ceiling) {
    attenuation_q16 =
        (static_cast<std::uint64_t>(positive_ceiling) * kQ16One) /
        maximum_positive;
  }
  if (maximum_negative_magnitude > negative_ceiling_magnitude) {
    const std::uint64_t negative_attenuation_q16 =
        (static_cast<std::uint64_t>(negative_ceiling_magnitude) * kQ16One) /
        maximum_negative_magnitude;
    if (negative_attenuation_q16 < attenuation_q16) {
      attenuation_q16 = negative_attenuation_q16;
    }
  }
  return static_cast<std::uint32_t>(attenuation_q16);
}

std::uint32_t PositiveCeilingFromPercent(
    const std::uint32_t percent) noexcept {
  constexpr std::uint32_t kPositiveFullScale =
      static_cast<std::uint32_t>(std::numeric_limits<std::int16_t>::max());
  return (kPositiveFullScale * percent + (kPercentScale / 2U)) /
         kPercentScale;
}

std::uint32_t NegativeCeilingFromPercent(
    const std::uint32_t percent) noexcept {
  constexpr std::uint32_t kNegativeFullScaleMagnitude = 32768U;
  return (kNegativeFullScaleMagnitude * percent +
          (kPercentScale / 2U)) /
         kPercentScale;
}

}  // namespace

Status PlaybackGainLimiter::Create(
    const PlaybackGainLimiterConfig& config,
    PlaybackGainLimiter* const output) {
  if (output == nullptr) {
    return Status::Error(StatusCode::kInvalidArgument,
                         "playback gain output must not be null");
  }
  if (output->configured_) {
    return Status::Error(StatusCode::kFailedPrecondition,
                         "playback gain is already configured");
  }
  if (config.volume_percent > 100U) {
    return Status::Error(StatusCode::kInvalidArgument,
                         "playback volume must be between 0 and 100");
  }
  if (config.speaker_gain_percent > kMaximumSpeakerGainPercent) {
    return Status::Error(StatusCode::kInvalidArgument,
                         "playback speaker gain exceeds 400 percent");
  }
  if (config.duck_target_percent > 100U) {
    return Status::Error(StatusCode::kInvalidArgument,
                         "playback duck target must be between 0 and 100");
  }
  if (config.duck_ramp_ms > kMaximumRampMs ||
      config.recovery_ramp_ms > kMaximumRampMs) {
    return Status::Error(StatusCode::kInvalidArgument,
                         "playback gain ramp exceeds 1000 ms");
  }
  if (config.limiter_ceiling_percent == 0U ||
      config.limiter_ceiling_percent > 100U) {
    return Status::Error(StatusCode::kInvalidArgument,
                         "playback limiter ceiling must be between 1 and 100");
  }
  if (config.limiter_release_ms > kMaximumRampMs) {
    return Status::Error(StatusCode::kInvalidArgument,
                         "playback limiter release exceeds 1000 ms");
  }

  const std::uint32_t volume_q16 = PercentToQ16(config.volume_percent);
  const std::uint32_t speaker_gain_q16 =
      PercentToQ16(config.speaker_gain_percent);
  output->base_gain_q16_ =
      MultiplyQ16Bounded(volume_q16, speaker_gain_q16);
  output->duck_target_q16_ = PercentToQ16(config.duck_target_percent);
  output->duck_ramp_samples_ =
      static_cast<std::uint32_t>(config.duck_ramp_ms) *
      kSamplesPerMillisecond;
  output->recovery_ramp_samples_ =
      static_cast<std::uint32_t>(config.recovery_ramp_ms) *
      kSamplesPerMillisecond;
  output->limiter_release_samples_ =
      static_cast<std::uint32_t>(config.limiter_release_ms) *
      kSamplesPerMillisecond;
  output->positive_ceiling_ =
      PositiveCeilingFromPercent(config.limiter_ceiling_percent);
  output->negative_ceiling_magnitude_ =
      NegativeCeilingFromPercent(config.limiter_ceiling_percent);
  output->ResetEnvelope();
  output->configured_ = true;
  return Status::Ok();
}

Status PlaybackGainLimiter::Arm(const PlaybackGeneration& generation) {
  if (!configured_) {
    return Status::Error(StatusCode::kFailedPrecondition,
                         "playback gain is not configured");
  }
  if (!generation.valid()) {
    return Status::Error(StatusCode::kInvalidArgument,
                         "playback gain generation must be complete");
  }
  if (armed_) {
    return Status::Error(
        StatusCode::kFailedPrecondition,
        "playback gain must be disarmed before changing generation");
  }
  if (generation.epoch == last_generation_.epoch &&
      generation.stream_id == last_generation_.stream_id) {
    return Status::Error(StatusCode::kFailedPrecondition,
                         "playback gain generation must not be reused");
  }

  active_generation_ = generation;
  last_generation_ = generation;
  armed_ = true;
  faulted_ = false;
  ResetEnvelope();
  return Status::Ok();
}

void PlaybackGainLimiter::Disarm() noexcept {
  active_generation_ = PlaybackGeneration{};
  armed_ = false;
  faulted_ = false;
  ResetEnvelope();
}

Status PlaybackGainLimiter::SetDucked(const bool ducked) {
  if (!configured_ || !armed_ || faulted_) {
    return Status::Error(StatusCode::kFailedPrecondition,
                         "playback gain cannot change duck state");
  }
  if (duck_requested_ == ducked) {
    return Status::Ok();
  }

  duck_requested_ = ducked;
  ramp_start_q16_ = current_envelope_q16_;
  ramp_target_q16_ = ducked ? duck_target_q16_ : kQ16One;
  ramp_total_samples_ =
      ducked ? duck_ramp_samples_ : recovery_ramp_samples_;
  ramp_elapsed_samples_ = 0U;
  if (ramp_total_samples_ == 0U ||
      ramp_start_q16_ == ramp_target_q16_) {
    current_envelope_q16_ = ramp_target_q16_;
    ramp_total_samples_ = 0U;
  }
  return Status::Ok();
}

PlaybackGainLimiterResult PlaybackGainLimiter::Process(
    const ResampledPcmFrame48k& input,
    PlaybackPcmFrame48k* const output) noexcept {
  if (output == nullptr) {
    return MakeResult(PlaybackGainLimiterCode::kInvalidOutput);
  }
  output->ResetHeader();
  if (!configured_) {
    return MakeResult(PlaybackGainLimiterCode::kNotConfigured);
  }
  if (!armed_) {
    return MakeResult(PlaybackGainLimiterCode::kNotArmed);
  }
  if (!input.HasValidLength()) {
    return MakeResult(PlaybackGainLimiterCode::kInvalidInput);
  }
  if (input.metadata.epoch != active_generation_.epoch) {
    return MakeResult(PlaybackGainLimiterCode::kEpochMismatch);
  }
  if (input.metadata.stream_id != active_generation_.stream_id) {
    return MakeResult(PlaybackGainLimiterCode::kStreamMismatch);
  }
  if (input.metadata.turn_id != active_generation_.turn_id) {
    return MakeResult(PlaybackGainLimiterCode::kTurnMismatch);
  }
  if (input.metadata.discontinuity) {
    faulted_ = true;
    return MakeResult(PlaybackGainLimiterCode::kDiscontinuity);
  }
  if (faulted_) {
    return MakeResult(PlaybackGainLimiterCode::kFaulted);
  }

  std::uint32_t over_full_scale_samples = 0U;
  std::uint64_t maximum_positive = 0U;
  std::uint64_t maximum_negative_magnitude = 0U;
  std::uint32_t preview_envelope_q16 = current_envelope_q16_;
  std::uint32_t preview_ramp_total_samples = ramp_total_samples_;
  std::uint32_t preview_ramp_elapsed_samples = ramp_elapsed_samples_;
  for (std::size_t sample_index = 0U;
       sample_index < input.valid_samples; ++sample_index) {
    AdvanceEnvelopeState(
        ramp_start_q16_, ramp_target_q16_, &preview_envelope_q16,
        &preview_ramp_total_samples, &preview_ramp_elapsed_samples);
    const std::uint32_t combined_gain_q16 =
        MultiplyQ16Bounded(base_gain_q16_, preview_envelope_q16);
    const std::int64_t scaled =
        ScaleSampleQ16(input.samples[sample_index], combined_gain_q16);
    if (scaled > std::numeric_limits<std::int16_t>::max() ||
        scaled < std::numeric_limits<std::int16_t>::min()) {
      ++over_full_scale_samples;
    }
    if (scaled > 0) {
      const auto magnitude = static_cast<std::uint64_t>(scaled);
      if (magnitude > maximum_positive) {
        maximum_positive = magnitude;
      }
    } else if (scaled < 0) {
      const auto magnitude = static_cast<std::uint64_t>(-scaled);
      if (magnitude > maximum_negative_magnitude) {
        maximum_negative_magnitude = magnitude;
      }
    }
  }

  const std::uint32_t required_limiter_gain_q16 =
      ComputeBlockAttenuationQ16(
          maximum_positive, maximum_negative_magnitude, positive_ceiling_,
          negative_ceiling_magnitude_);
  PrepareLimiterForChunk(required_limiter_gain_q16);
  std::uint32_t limited_samples = 0U;
  std::uint32_t safety_saturated_samples = 0U;
  const auto positive_ceiling =
      static_cast<std::int64_t>(positive_ceiling_);
  const auto negative_ceiling =
      -static_cast<std::int64_t>(negative_ceiling_magnitude_);
  for (std::size_t sample_index = 0U;
       sample_index < input.valid_samples; ++sample_index) {
    AdvanceLimiterOneSample(required_limiter_gain_q16);
    if (limiter_gain_q16_ < kQ16One) {
      ++limited_samples;
    }
    AdvanceEnvelopeOneSample();
    const std::uint32_t combined_gain_q16 =
        MultiplyQ16Bounded(base_gain_q16_, current_envelope_q16_);
    const std::int64_t scaled =
        ScaleSampleQ16(input.samples[sample_index], combined_gain_q16);
    const std::int64_t limited =
        ScaleSignedValueQ16(scaled, limiter_gain_q16_);
    if (limited > positive_ceiling) {
      output->samples[sample_index] =
          static_cast<std::int16_t>(positive_ceiling);
      ++safety_saturated_samples;
    } else if (limited < negative_ceiling) {
      output->samples[sample_index] =
          static_cast<std::int16_t>(negative_ceiling);
      ++safety_saturated_samples;
    } else {
      output->samples[sample_index] = static_cast<std::int16_t>(limited);
    }
  }
  for (std::size_t sample_index = input.valid_samples;
       sample_index < output->samples.size(); ++sample_index) {
    output->samples[sample_index] = 0;
  }

  output->format.sample_rate_hz = PlaybackPcmFrame48k::kSampleRateHz;
  output->format.frame_duration_ms =
      PlaybackPcmFrame48k::kFrameDurationMs;
  output->format.channels = PlaybackPcmFrame48k::kChannelCount;
  output->format.sample_format = PlaybackPcmFrame48k::kSampleFormat;
  output->metadata = input.metadata;
  output->source_offset_sample_frames =
      input.source_offset_sample_frames;
  output->valid_samples = input.valid_samples;
  output->end_of_stream = input.end_of_stream;
  if (input.end_of_stream) {
    Disarm();
  }
  return MakeResult(PlaybackGainLimiterCode::kProduced,
                    over_full_scale_samples, limited_samples,
                    safety_saturated_samples);
}

void PlaybackGainLimiter::ResetEnvelope() noexcept {
  current_envelope_q16_ = kQ16One;
  ramp_start_q16_ = kQ16One;
  ramp_target_q16_ = kQ16One;
  ramp_total_samples_ = 0U;
  ramp_elapsed_samples_ = 0U;
  duck_requested_ = false;
  limiter_gain_q16_ = kQ16One;
  limiter_release_start_q16_ = kQ16One;
  limiter_release_elapsed_samples_ = 0U;
}

void PlaybackGainLimiter::AdvanceEnvelopeOneSample() noexcept {
  AdvanceEnvelopeState(ramp_start_q16_, ramp_target_q16_,
                       &current_envelope_q16_, &ramp_total_samples_,
                       &ramp_elapsed_samples_);
}

void PlaybackGainLimiter::PrepareLimiterForChunk(
    const std::uint32_t required_gain_q16) noexcept {
  if (required_gain_q16 < limiter_gain_q16_) {
    // Peak limiting has zero lookahead beyond the already available chunk:
    // attack before its first sample so no value can cross the ceiling.
    limiter_gain_q16_ = required_gain_q16;
    limiter_release_start_q16_ = required_gain_q16;
    limiter_release_elapsed_samples_ = 0U;
    return;
  }
  if (limiter_release_samples_ == 0U) {
    limiter_gain_q16_ = required_gain_q16;
    limiter_release_start_q16_ = required_gain_q16;
    limiter_release_elapsed_samples_ = 0U;
  }
}

void PlaybackGainLimiter::AdvanceLimiterOneSample(
    const std::uint32_t required_gain_q16) noexcept {
  if (limiter_gain_q16_ >= kQ16One || limiter_release_samples_ == 0U ||
      limiter_release_elapsed_samples_ >= limiter_release_samples_) {
    return;
  }

  const std::uint32_t next_elapsed =
      limiter_release_elapsed_samples_ + 1U;
  const std::uint32_t distance =
      kQ16One - limiter_release_start_q16_;
  const auto numerator =
      static_cast<std::uint64_t>(distance) * next_elapsed +
      (limiter_release_samples_ / 2U);
  const std::uint32_t traversed =
      static_cast<std::uint32_t>(numerator / limiter_release_samples_);
  const std::uint32_t candidate =
      limiter_release_start_q16_ + traversed;
  if (candidate > required_gain_q16) {
    // Pause rather than consuming release time behind the chunk ceiling. A
    // later quieter chunk resumes without a gain jump or hidden recovery.
    return;
  }

  limiter_gain_q16_ = candidate;
  limiter_release_elapsed_samples_ = next_elapsed;
  if (limiter_release_elapsed_samples_ == limiter_release_samples_) {
    limiter_gain_q16_ = kQ16One;
  }
}

}  // namespace boompi::audio
