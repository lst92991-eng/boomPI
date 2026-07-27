#include "boompi/audio/playback_resampler_24_to_48.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace boompi::audio {
namespace {

constexpr std::int64_t kQ31Scale = INT64_C(2147483648);
constexpr std::int64_t kQ31Half = kQ31Scale / 2;

// Reproducible design: 65-tap type-I half-band FIR at 48 kHz, cutoff 12 kHz,
// Kaiser beta 7.85726. For offset m from the centre, the unwindowed gain-2
// interpolator is 2*sin(pi*m/2)/(pi*m), with h[32] = 1. The non-centre taps
// are normalized to sum to one, then quantized to nearest Q31. The centre is
// INT32_MAX, giving total DC gain 2 - 2^-31. Even non-centre taps remain
// exactly zero. The resulting response is approximately +0.0011 dB at
// 10 kHz and -78.2 dB at the 14 kHz image edge.
constexpr std::array<std::int32_t, 65U> kCoefficientsQ31{{
    0,          -259341,    0,          800800,      0,
    -1846783,   0,          3656617,    0,           -6559276,
    0,          10961781,   0,          -17364505,   0,
    26393630,   0,          -38871571,  0,           55970269,
    0,          -79554954,  0,          113013354,   0,
    -163534336, 0,          249878515,  0,           -441236499,
    0,          1362294123, 2147483647, 1362294123,  0,
    -441236499, 0,          249878515,  0,           -163534336,
    0,          113013354,  0,          -79554954,   0,
    55970269,   0,          -38871571,  0,           26393630,
    0,          -17364505,  0,          10961781,    0,
    -6559276,   0,          3656617,    0,           -1846783,
    0,          800800,     0,          -259341,     0,
}};

constexpr bool CoefficientsAreValid() noexcept {
  constexpr std::size_t kMidpoint =
      (PlaybackResampler24To48::kTapCount - 1U) / 2U;
  std::int64_t sum = 0;
  for (std::size_t tap = 0U; tap < kCoefficientsQ31.size(); ++tap) {
    sum += kCoefficientsQ31[tap];
    if (kCoefficientsQ31[tap] !=
        kCoefficientsQ31[kCoefficientsQ31.size() - 1U - tap]) {
      return false;
    }
    if (tap != kMidpoint && tap % 2U == 0U &&
        kCoefficientsQ31[tap] != 0) {
      return false;
    }
  }
  return sum == 2 * kQ31Scale - 1;
}

constexpr std::int64_t CoefficientAbsoluteSum() noexcept {
  std::int64_t absolute_sum = 0;
  for (const std::int32_t coefficient : kCoefficientsQ31) {
    const std::int64_t widened = coefficient;
    absolute_sum += widened < 0 ? -widened : widened;
  }
  return absolute_sum;
}

static_assert(kCoefficientsQ31.size() ==
                  PlaybackResampler24To48::kTapCount,
              "interpolator coefficient count changed");
static_assert(CoefficientsAreValid(),
              "interpolator coefficients must be symmetric half-band Q31");
static_assert(PlaybackResampler24To48::kHistorySamples + 1U ==
                  PlaybackResampler24To48::kOddPhaseTapCount,
              "odd polyphase history extent changed");
static_assert(TtsPcmFrame24k::kFrameSamples *
                      PlaybackResampler24To48::kInterpolationFactor ==
                  ResampledPcmFrame48k::kFrameSamples,
              "24 kHz and 48 kHz frames must have an exact 2:1 ratio");
static_assert(ResampledPcmFrame48k::kFrameSamples +
                      PlaybackResampler24To48::kDrainSamples ==
                  ResampledPcmFrame48k::kMaximumSourceSpanSampleFrames,
              "wide PCM span must contain one full prefix and FIR drain");
static_assert(PlaybackPcmFrame48k::kFrameSamples +
                      PlaybackResampler24To48::kDrainSamples ==
                  PlaybackPcmFrame48k::kMaximumSourceSpanSampleFrames,
              "final PCM span must contain one full prefix and FIR drain");
constexpr std::int64_t kWorstAccumulatorMagnitude =
    CoefficientAbsoluteSum() *
    -static_cast<std::int64_t>(
        std::numeric_limits<std::int16_t>::min());
static_assert(kWorstAccumulatorMagnitude <=
                  std::numeric_limits<std::int64_t>::max() - kQ31Half,
              "worst-case Q31 accumulator plus rounding headroom must fit");
static_assert((kWorstAccumulatorMagnitude + kQ31Half) / kQ31Scale <=
                  std::numeric_limits<std::int32_t>::max(),
              "rounded FIR overshoot must fit the wide PCM sample type");

std::int32_t RoundQ31(const std::int64_t accumulator) noexcept {
  // Deterministic nearest rounding; exact half-LSB ties move away from zero.
  std::int64_t rounded = 0;
  if (accumulator >= 0) {
    rounded = (accumulator + kQ31Half) / kQ31Scale;
  } else {
    rounded = -((-accumulator + kQ31Half) / kQ31Scale);
  }
  return static_cast<std::int32_t>(rounded);
}

PlaybackGeneration GenerationOf(const TtsPcmFrame24k& frame) noexcept {
  return PlaybackGeneration{frame.metadata.epoch, frame.metadata.turn_id,
                            frame.metadata.stream_id};
}

}  // namespace

Status PlaybackResampler24To48::Arm(
    const PlaybackGeneration& generation) {
  if (!generation.valid()) {
    return Status::Error(
        StatusCode::kInvalidArgument,
        "playback resampler generation ids must all be non-zero");
  }
  if (armed_) {
    return Status::Error(
        StatusCode::kFailedPrecondition,
        "playback resampler must be disarmed before a new generation");
  }

  const Status continuity_status =
      continuity_gate_.Arm(generation.epoch, generation.stream_id);
  if (!continuity_status.ok()) {
    return continuity_status;
  }

  active_generation_ = generation;
  armed_ = true;
  ClearDspState();
  return Status::Ok();
}

void PlaybackResampler24To48::Disarm() noexcept {
  continuity_gate_.Disarm();
  active_generation_ = PlaybackGeneration{};
  armed_ = false;
  ClearDspState();
}

PlaybackResampleResult PlaybackResampler24To48::Process(
    const TtsPcmFrame24k& input,
    ResampledPcmFrame48k* const output) noexcept {
  if (output == nullptr) {
    return {PlaybackResampleCode::kInvalidArgument, drain_pending_};
  }
  output->ResetHeader();

  const PlaybackGeneration input_generation = GenerationOf(input);
  if (!input_generation.valid()) {
    return {PlaybackResampleCode::kInvalidGeneration, drain_pending_};
  }
  if (!armed_) {
    return {PlaybackResampleCode::kNotArmed, drain_pending_};
  }
  if (input_generation.epoch != active_generation_.epoch) {
    return {PlaybackResampleCode::kEpochMismatch, drain_pending_};
  }
  if (input_generation.stream_id != active_generation_.stream_id) {
    return {PlaybackResampleCode::kStreamMismatch, drain_pending_};
  }
  if (input_generation.turn_id != active_generation_.turn_id) {
    return {PlaybackResampleCode::kTurnMismatch, drain_pending_};
  }
  if (drain_pending_) {
    return {PlaybackResampleCode::kDrainPending, true};
  }
  if (transform_faulted_) {
    return {PlaybackResampleCode::kFaulted, false};
  }
  if (!input.HasValidLength()) {
    transform_faulted_ = true;
    return {PlaybackResampleCode::kInvalidInputFrame, false};
  }

  const FrameContinuityResult continuity =
      continuity_gate_.CheckAndAdvance(input.metadata);
  switch (continuity) {
    case FrameContinuityResult::kAcceptedFirst:
    case FrameContinuityResult::kAcceptedContinuous:
      break;
    case FrameContinuityResult::kNotArmed:
      return {PlaybackResampleCode::kNotArmed, false};
    case FrameContinuityResult::kEpochMismatch:
      return {PlaybackResampleCode::kEpochMismatch, false};
    case FrameContinuityResult::kStreamMismatch:
      return {PlaybackResampleCode::kStreamMismatch, false};
    case FrameContinuityResult::kDiscontinuity:
      transform_faulted_ = true;
      return {PlaybackResampleCode::kDiscontinuity, false};
    case FrameContinuityResult::kSequenceBreak:
      transform_faulted_ = true;
      return {PlaybackResampleCode::kSequenceBreak, false};
    case FrameContinuityResult::kTimestampNotIncreasing:
      transform_faulted_ = true;
      return {PlaybackResampleCode::kTimestampNotIncreasing, false};
    case FrameContinuityResult::kFaulted:
      transform_faulted_ = true;
      return {PlaybackResampleCode::kFaulted, false};
  }

  output->samples.fill(0);
  const std::size_t valid_source_samples = input.valid_source_samples;
  constexpr std::size_t kCentreDelay = kLatencyInputSamples;
  constexpr std::size_t kCentreTap = (kTapCount - 1U) / 2U;

  for (std::size_t source_index = 0U;
       source_index < valid_source_samples; ++source_index) {
    const std::int64_t even_accumulator =
        static_cast<std::int64_t>(kCoefficientsQ31[kCentreTap]) *
        SourceSample(input, source_index, kCentreDelay);
    output->samples[source_index * kInterpolationFactor] =
        RoundQ31(even_accumulator);

    std::int64_t odd_accumulator = 0;
    for (std::size_t phase_tap = 0U; phase_tap < kOddPhaseTapCount;
         ++phase_tap) {
      const std::size_t full_tap = phase_tap * kInterpolationFactor + 1U;
      odd_accumulator +=
          static_cast<std::int64_t>(kCoefficientsQ31[full_tap]) *
          SourceSample(input, source_index, phase_tap);
    }
    output->samples[source_index * kInterpolationFactor + 1U] =
        RoundQ31(odd_accumulator);
  }

  UpdateHistory(input);

  output->metadata = input.metadata;
  output->source_offset_sample_frames = 0U;
  output->valid_samples = static_cast<std::uint16_t>(
      valid_source_samples * kInterpolationFactor);
  output->end_of_stream = false;
  if (input.end_of_stream) {
    drain_metadata_ = input.metadata;
    drain_source_offset_ = output->valid_samples;
    drain_pending_ = true;
  }
  return {PlaybackResampleCode::kProduced, input.end_of_stream};
}

PlaybackResampleResult PlaybackResampler24To48::Drain(
    ResampledPcmFrame48k* const output) noexcept {
  if (output == nullptr) {
    return {PlaybackResampleCode::kInvalidArgument, drain_pending_};
  }
  output->ResetHeader();
  if (!armed_) {
    return {PlaybackResampleCode::kNotArmed, false};
  }
  if (transform_faulted_) {
    return {PlaybackResampleCode::kFaulted, false};
  }
  if (!drain_pending_) {
    return {PlaybackResampleCode::kNoDrainPending, false};
  }

  output->samples.fill(0);
  constexpr std::size_t kCentreDelay = kLatencyInputSamples;
  constexpr std::size_t kCentreTap = (kTapCount - 1U) / 2U;
  constexpr std::size_t kZeroSourceSteps = (kDrainSamples - 1U) / 2U;
  for (std::size_t zero_source_index = 0U;
       zero_source_index < kZeroSourceSteps; ++zero_source_index) {
    const std::int64_t even_accumulator =
        static_cast<std::int64_t>(kCoefficientsQ31[kCentreTap]) *
        DrainSourceSample(zero_source_index, kCentreDelay);
    output->samples[zero_source_index * kInterpolationFactor] =
        RoundQ31(even_accumulator);

    std::int64_t odd_accumulator = 0;
    for (std::size_t phase_tap = 0U; phase_tap < kOddPhaseTapCount;
         ++phase_tap) {
      const std::size_t full_tap = phase_tap * kInterpolationFactor + 1U;
      odd_accumulator +=
          static_cast<std::int64_t>(kCoefficientsQ31[full_tap]) *
          DrainSourceSample(zero_source_index, phase_tap);
    }
    output->samples[zero_source_index * kInterpolationFactor + 1U] =
        RoundQ31(odd_accumulator);
  }
  // Tap 64 is an exact half-band zero, but retain its output position so the
  // drain is the complete 63-position convolution suffix for every EOS N.
  output->samples[kDrainSamples - 1U] = 0;
  output->metadata = drain_metadata_;
  output->source_offset_sample_frames = drain_source_offset_;
  output->valid_samples = static_cast<std::uint16_t>(kDrainSamples);
  output->end_of_stream = true;

  Disarm();
  return {PlaybackResampleCode::kProduced, false};
}

void PlaybackResampler24To48::ClearDspState() noexcept {
  history_.fill(0);
  drain_metadata_ = AudioFrameMetadata{};
  drain_source_offset_ = 0U;
  transform_faulted_ = false;
  drain_pending_ = false;
}

std::int16_t PlaybackResampler24To48::SourceSample(
    const TtsPcmFrame24k& input, const std::size_t source_index,
    const std::size_t delay) const noexcept {
  if (delay <= source_index) {
    return input.samples[source_index - delay];
  }
  return history_[kHistorySamples + source_index - delay];
}

std::int16_t PlaybackResampler24To48::DrainSourceSample(
    const std::size_t zero_source_index,
    const std::size_t delay) const noexcept {
  if (delay <= zero_source_index) {
    return 0;
  }
  return history_[kHistorySamples + zero_source_index - delay];
}

void PlaybackResampler24To48::UpdateHistory(
    const TtsPcmFrame24k& input) noexcept {
  const std::size_t valid_source_samples = input.valid_source_samples;
  if (valid_source_samples >= kHistorySamples) {
    const std::size_t source_start =
        valid_source_samples - kHistorySamples;
    for (std::size_t index = 0U; index < kHistorySamples; ++index) {
      history_[index] = input.samples[source_start + index];
    }
    return;
  }

  const std::size_t retained_samples =
      kHistorySamples - valid_source_samples;
  for (std::size_t index = 0U; index < retained_samples; ++index) {
    history_[index] = history_[index + valid_source_samples];
  }
  for (std::size_t index = 0U; index < valid_source_samples; ++index) {
    history_[retained_samples + index] = input.samples[index];
  }
}

}  // namespace boompi::audio
