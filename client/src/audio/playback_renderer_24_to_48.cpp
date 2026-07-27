#include "boompi/audio/playback_renderer_24_to_48.h"

#include <cstdint>
#include <type_traits>

namespace boompi::audio {
namespace {

static_assert(std::is_trivial<PlaybackRenderResult>::value,
              "playback render result must remain a POD value");
static_assert(std::is_standard_layout<PlaybackRenderResult>::value,
              "playback render result must remain standard-layout");
static_assert(TtsPcmFrame24k::kFrameSamples *
                      PlaybackResampler24To48::kInterpolationFactor ==
                  PlaybackPcmFrame48k::kFrameSamples,
              "renderer requires exact fixed 24-to-48 kHz frame sizing");

constexpr PlaybackRenderResult MakeResult(
    const PlaybackRenderCode code,
    const std::uint32_t over_full_scale_samples = 0U,
    const std::uint32_t limited_samples = 0U,
    const std::uint32_t safety_saturated_samples = 0U,
    const bool drain_required = false) noexcept {
  return {code, over_full_scale_samples, limited_samples,
          safety_saturated_samples, drain_required};
}

bool SameEpochAndStream(const PlaybackGeneration& lhs,
                        const PlaybackGeneration& rhs) noexcept {
  return lhs.epoch == rhs.epoch && lhs.stream_id == rhs.stream_id;
}

bool MetadataEqual(const AudioFrameMetadata& lhs,
                   const AudioFrameMetadata& rhs) noexcept {
  return lhs.monotonic_timestamp_us == rhs.monotonic_timestamp_us &&
         lhs.sequence == rhs.sequence && lhs.stream_id == rhs.stream_id &&
         lhs.turn_id == rhs.turn_id && lhs.epoch == rhs.epoch &&
         lhs.discontinuity == rhs.discontinuity;
}

PlaybackRenderCode MapResamplerFailure(
    const PlaybackResampleCode code) noexcept {
  switch (code) {
    case PlaybackResampleCode::kInvalidInputFrame:
      return PlaybackRenderCode::kInvalidInputFrame;
    case PlaybackResampleCode::kDiscontinuity:
      return PlaybackRenderCode::kDiscontinuity;
    case PlaybackResampleCode::kSequenceBreak:
      return PlaybackRenderCode::kSequenceBreak;
    case PlaybackResampleCode::kTimestampNotIncreasing:
      return PlaybackRenderCode::kTimestampNotIncreasing;
    case PlaybackResampleCode::kDrainPending:
    case PlaybackResampleCode::kNoDrainPending:
      return PlaybackRenderCode::kStageFault;
    case PlaybackResampleCode::kFaulted:
      return PlaybackRenderCode::kFaulted;
    case PlaybackResampleCode::kProduced:
    case PlaybackResampleCode::kInvalidArgument:
    case PlaybackResampleCode::kNotArmed:
    case PlaybackResampleCode::kInvalidGeneration:
    case PlaybackResampleCode::kEpochMismatch:
    case PlaybackResampleCode::kStreamMismatch:
    case PlaybackResampleCode::kTurnMismatch:
      return PlaybackRenderCode::kStageFault;
  }
  return PlaybackRenderCode::kStageFault;
}

}  // namespace

Status PlaybackRenderer24To48::Create(
    const PlaybackGainLimiterConfig& gain_config,
    PlaybackRenderer24To48* const output) {
  if (output == nullptr) {
    return Status::Error(StatusCode::kInvalidArgument,
                         "playback renderer output must not be null");
  }
  if (output->configured_) {
    return Status::Error(StatusCode::kFailedPrecondition,
                         "playback renderer is already configured");
  }

  const Status gain_status =
      PlaybackGainLimiter::Create(gain_config, &output->gain_limiter_);
  if (!gain_status.ok()) {
    return gain_status;
  }
  output->configured_ = true;
  return Status::Ok();
}

Status PlaybackRenderer24To48::Arm(
    const PlaybackGeneration& generation) {
  if (!configured_) {
    return Status::Error(StatusCode::kFailedPrecondition,
                         "playback renderer is not configured");
  }
  if (!generation.valid()) {
    return Status::Error(StatusCode::kInvalidArgument,
                         "playback renderer generation must be complete");
  }
  if (drain_pending_) {
    return Status::Error(StatusCode::kFailedPrecondition,
                         "playback renderer drain must complete first");
  }
  if (armed_) {
    return Status::Error(
        StatusCode::kFailedPrecondition,
        "playback renderer must be disarmed before changing generation");
  }
  if (SameEpochAndStream(generation, last_generation_)) {
    return Status::Error(StatusCode::kFailedPrecondition,
                         "playback renderer generation must not be reused");
  }

  const Status resampler_status = resampler_.Arm(generation);
  if (!resampler_status.ok()) {
    last_generation_ = generation;
    LatchStageFault();
    return Status::Error(StatusCode::kInternal,
                         "playback renderer could not arm resampler stage");
  }

  const Status gain_status = gain_limiter_.Arm(generation);
  if (!gain_status.ok()) {
    last_generation_ = generation;
    LatchStageFault();
    return Status::Error(StatusCode::kInternal,
                         "playback renderer could not arm gain stage");
  }

  active_generation_ = generation;
  last_generation_ = generation;
  armed_ = true;
  drain_pending_ = false;
  faulted_ = false;
  resampled_scratch_.ResetHeader();
  rendered_scratch_.ResetHeader();
  drain_metadata_ = AudioFrameMetadata{};
  drain_source_offset_sample_frames_ = 0U;
  return Status::Ok();
}

void PlaybackRenderer24To48::Disarm() noexcept {
  resampler_.Disarm();
  gain_limiter_.Disarm();
  resampled_scratch_.ResetHeader();
  rendered_scratch_.ResetHeader();
  drain_metadata_ = AudioFrameMetadata{};
  drain_source_offset_sample_frames_ = 0U;
  active_generation_ = PlaybackGeneration{};
  armed_ = false;
  drain_pending_ = false;
}

Status PlaybackRenderer24To48::SetDucked(const bool ducked) {
  if (!configured_ || !armed_ || drain_pending_ || faulted_) {
    return Status::Error(StatusCode::kFailedPrecondition,
                         "playback renderer cannot change duck state");
  }

  const Status gain_status = gain_limiter_.SetDucked(ducked);
  if (!gain_status.ok()) {
    LatchStageFault();
    return Status::Error(StatusCode::kInternal,
                         "playback renderer gain stage rejected duck state");
  }
  return Status::Ok();
}

PlaybackRenderResult PlaybackRenderer24To48::Process(
    const TtsPcmFrame24k& input,
    PlaybackPcmFrame48k* const output) noexcept {
  if (output == nullptr) {
    return MakeResult(PlaybackRenderCode::kInvalidOutput, 0U, 0U, 0U,
                      drain_pending_);
  }
  output->ResetHeader();
  if (!configured_) {
    return MakeResult(PlaybackRenderCode::kNotConfigured);
  }

  const PlaybackGeneration input_generation{
      input.metadata.epoch, input.metadata.turn_id,
      input.metadata.stream_id};
  if (!input_generation.valid()) {
    return MakeResult(PlaybackRenderCode::kInvalidGeneration, 0U, 0U, 0U,
                      drain_pending_);
  }
  if (faulted_) {
    return MakeResult(PlaybackRenderCode::kFaulted, 0U, 0U, 0U,
                      drain_pending_);
  }
  if (!armed_) {
    return MakeResult(PlaybackRenderCode::kNotArmed, 0U, 0U, 0U,
                      drain_pending_);
  }
  if (input_generation.epoch != active_generation_.epoch) {
    return MakeResult(PlaybackRenderCode::kEpochMismatch, 0U, 0U, 0U,
                      drain_pending_);
  }
  if (input_generation.stream_id != active_generation_.stream_id) {
    return MakeResult(PlaybackRenderCode::kStreamMismatch, 0U, 0U, 0U,
                      drain_pending_);
  }
  if (input_generation.turn_id != active_generation_.turn_id) {
    return MakeResult(PlaybackRenderCode::kTurnMismatch, 0U, 0U, 0U,
                      drain_pending_);
  }
  if (drain_pending_) {
    return MakeResult(PlaybackRenderCode::kDrainPending, 0U, 0U, 0U,
                      true);
  }

  const PlaybackResampleResult resample_result =
      resampler_.Process(input, &resampled_scratch_);
  if (!resample_result.produced()) {
    const PlaybackRenderCode render_code =
        MapResamplerFailure(resample_result.code);
    LatchStageFault();
    return MakeResult(render_code);
  }
  if (resample_result.drain_required != input.end_of_stream) {
    LatchStageFault();
    return MakeResult(PlaybackRenderCode::kStageFault);
  }

  const PlaybackGainLimiterResult gain_result =
      gain_limiter_.Process(resampled_scratch_, &rendered_scratch_);
  if (!gain_result.produced()) {
    rendered_scratch_.ResetHeader();
    LatchStageFault();
    return MakeResult(PlaybackRenderCode::kStageFault);
  }

  const auto expected_valid_samples = static_cast<std::uint16_t>(
      input.valid_source_samples *
      PlaybackResampler24To48::kInterpolationFactor);
  if (!rendered_scratch_.HasValidLength() ||
      rendered_scratch_.valid_samples != expected_valid_samples ||
      rendered_scratch_.source_offset_sample_frames != 0U ||
      rendered_scratch_.end_of_stream ||
      !MetadataEqual(rendered_scratch_.metadata, input.metadata)) {
    rendered_scratch_.ResetHeader();
    LatchStageFault();
    return MakeResult(PlaybackRenderCode::kStageFault,
                      gain_result.over_full_scale_samples,
                      gain_result.limited_samples,
                      gain_result.safety_saturated_samples);
  }

  if (input.end_of_stream) {
    drain_pending_ = true;
    drain_metadata_ = input.metadata;
    drain_source_offset_sample_frames_ = expected_valid_samples;
  }
  *output = rendered_scratch_;
  return MakeResult(PlaybackRenderCode::kProduced,
                    gain_result.over_full_scale_samples,
                    gain_result.limited_samples,
                    gain_result.safety_saturated_samples,
                    input.end_of_stream);
}

PlaybackRenderResult PlaybackRenderer24To48::Drain(
    PlaybackPcmFrame48k* const output) noexcept {
  if (output == nullptr) {
    return MakeResult(PlaybackRenderCode::kInvalidOutput, 0U, 0U, 0U,
                      drain_pending_);
  }
  output->ResetHeader();
  if (!configured_) {
    return MakeResult(PlaybackRenderCode::kNotConfigured);
  }
  if (faulted_) {
    return MakeResult(PlaybackRenderCode::kFaulted);
  }
  if (!armed_) {
    return MakeResult(PlaybackRenderCode::kNotArmed);
  }
  if (!drain_pending_) {
    return MakeResult(PlaybackRenderCode::kNoDrainPending);
  }

  const PlaybackResampleResult resample_result =
      resampler_.Drain(&resampled_scratch_);
  if (!resample_result.produced() || resample_result.drain_required) {
    const PlaybackRenderCode render_code =
        resample_result.produced()
            ? PlaybackRenderCode::kStageFault
            : MapResamplerFailure(resample_result.code);
    LatchStageFault();
    return MakeResult(render_code);
  }

  const PlaybackGainLimiterResult gain_result =
      gain_limiter_.Process(resampled_scratch_, &rendered_scratch_);
  if (!gain_result.produced()) {
    rendered_scratch_.ResetHeader();
    LatchStageFault();
    return MakeResult(PlaybackRenderCode::kStageFault);
  }

  const bool generation_matches =
      rendered_scratch_.metadata.epoch == active_generation_.epoch &&
      rendered_scratch_.metadata.stream_id == active_generation_.stream_id &&
      rendered_scratch_.metadata.turn_id == active_generation_.turn_id;
  if (!rendered_scratch_.HasValidLength() ||
      !rendered_scratch_.end_of_stream ||
      rendered_scratch_.valid_samples !=
          PlaybackResampler24To48::kDrainSamples ||
      rendered_scratch_.source_offset_sample_frames !=
          drain_source_offset_sample_frames_ ||
      !MetadataEqual(rendered_scratch_.metadata, drain_metadata_) ||
      !generation_matches) {
    rendered_scratch_.ResetHeader();
    LatchStageFault();
    return MakeResult(PlaybackRenderCode::kStageFault,
                      gain_result.over_full_scale_samples,
                      gain_result.limited_samples,
                      gain_result.safety_saturated_samples);
  }

  *output = rendered_scratch_;
  RetireSuccessfulDrain();
  return MakeResult(PlaybackRenderCode::kProduced,
                    gain_result.over_full_scale_samples,
                    gain_result.limited_samples,
                    gain_result.safety_saturated_samples);
}

void PlaybackRenderer24To48::LatchStageFault() noexcept {
  resampler_.Disarm();
  gain_limiter_.Disarm();
  resampled_scratch_.ResetHeader();
  rendered_scratch_.ResetHeader();
  drain_metadata_ = AudioFrameMetadata{};
  drain_source_offset_sample_frames_ = 0U;
  active_generation_ = PlaybackGeneration{};
  armed_ = false;
  drain_pending_ = false;
  faulted_ = true;
}

void PlaybackRenderer24To48::RetireSuccessfulDrain() noexcept {
  // Both child stages auto-retire after their successful drain/gain calls;
  // explicit Disarm keeps this invariant fail-closed if either implementation
  // changes while retaining each child's most-recent generation identity.
  resampler_.Disarm();
  gain_limiter_.Disarm();
  resampled_scratch_.ResetHeader();
  rendered_scratch_.ResetHeader();
  drain_metadata_ = AudioFrameMetadata{};
  drain_source_offset_sample_frames_ = 0U;
  active_generation_ = PlaybackGeneration{};
  armed_ = false;
  drain_pending_ = false;
  faulted_ = false;
}

}  // namespace boompi::audio
