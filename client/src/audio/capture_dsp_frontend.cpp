#include "boompi/audio/capture_dsp_frontend.h"

#include <type_traits>

namespace boompi::audio {
namespace {

static_assert(std::is_trivial<CaptureDspFrontendResult>::value,
              "capture DSP frontend result must remain a POD value");
static_assert(std::is_standard_layout<CaptureDspFrontendResult>::value,
              "capture DSP frontend result must remain standard-layout");

constexpr CaptureDspFrontendResult MakeResult(
    const CaptureDspFrontendCode code,
    const FrameContinuityResult continuity) noexcept {
  return {code, continuity, 0U, 0U};
}

}  // namespace

Status CaptureDspFrontend::Create(const ChannelMap& channel_map,
                                  CaptureDspFrontend* const output) {
  if (output == nullptr) {
    return Status::Error(StatusCode::kInvalidArgument,
                         "capture DSP frontend output must not be null");
  }
  if (output->configured_) {
    return Status::Error(
        StatusCode::kFailedPrecondition,
        "capture DSP frontend channel map is already configured");
  }

  CaptureChannelMapper candidate_mapper;
  const Status mapper_status =
      CaptureChannelMapper::Create(channel_map, &candidate_mapper);
  if (!mapper_status.ok()) {
    return mapper_status;
  }

  // No operation below can fail. Delay all writes until after validation so a
  // failed Create preserves the complete prior instance.
  output->mapper_ = candidate_mapper;
  output->decimator_.Reset();
  output->fault_latched_ = false;
  output->configured_ = true;
  return Status::Ok();
}

Status CaptureDspFrontend::Arm(const std::uint32_t epoch,
                               const std::uint32_t stream_id) {
  if (!configured_) {
    return Status::Error(StatusCode::kFailedPrecondition,
                         "capture DSP frontend is not configured");
  }

  const Status gate_status = continuity_gate_.Arm(epoch, stream_id);
  if (!gate_status.ok()) {
    return gate_status;
  }

  decimator_.Reset();
  fault_latched_ = false;
  return Status::Ok();
}

void CaptureDspFrontend::Disarm() noexcept {
  continuity_gate_.Disarm();
  decimator_.Reset();
  fault_latched_ = false;
}

CaptureDspFrontendResult CaptureDspFrontend::Process(
    const CaptureFrame& input, DspCaptureFrame16k* const output) noexcept {
  if (!configured_) {
    return MakeResult(CaptureDspFrontendCode::kNotConfigured,
                      FrameContinuityResult::kNotArmed);
  }
  if (output == nullptr) {
    return MakeResult(CaptureDspFrontendCode::kInvalidOutput,
                      FrameContinuityResult::kNotArmed);
  }

  const FrameContinuityResult continuity =
      continuity_gate_.CheckAndAdvance(input.metadata);
  switch (continuity) {
    case FrameContinuityResult::kNotArmed:
      return MakeResult(CaptureDspFrontendCode::kNotArmed, continuity);
    case FrameContinuityResult::kEpochMismatch:
      return MakeResult(CaptureDspFrontendCode::kEpochMismatch, continuity);
    case FrameContinuityResult::kStreamMismatch:
      return MakeResult(CaptureDspFrontendCode::kStreamMismatch, continuity);
    case FrameContinuityResult::kAcceptedFirst:
    case FrameContinuityResult::kAcceptedContinuous:
      break;
    case FrameContinuityResult::kDiscontinuity:
    case FrameContinuityResult::kSequenceBreak:
    case FrameContinuityResult::kTimestampNotIncreasing:
      if (!fault_latched_) {
        decimator_.Reset();
        fault_latched_ = true;
        return MakeResult(CaptureDspFrontendCode::kContinuityFault,
                          continuity);
      }
      return MakeResult(CaptureDspFrontendCode::kFaulted, continuity);
    case FrameContinuityResult::kFaulted:
      if (!fault_latched_) {
        decimator_.Reset();
        fault_latched_ = true;
      }
      return MakeResult(CaptureDspFrontendCode::kFaulted, continuity);
  }

  // The gate still runs while a transform fault is latched. This preserves
  // its non-mutating epoch/stream rejection semantics for queued stale frames,
  // while current-generation frames remain suppressed until a new Arm.
  if (fault_latched_) {
    return MakeResult(CaptureDspFrontendCode::kFaulted, continuity);
  }

  const SampleTransformResult mapper_result =
      mapper_.Process(input, &mapped_48k_);
  if (!mapper_result.ok()) {
    decimator_.Reset();
    fault_latched_ = true;
    CaptureDspFrontendResult result =
        MakeResult(CaptureDspFrontendCode::kTransformFault, continuity);
    result.mapper_saturated_samples = mapper_result.saturated_samples;
    return result;
  }

  const SampleTransformResult fir_result =
      decimator_.Process(mapped_48k_, &output->planes);
  if (!fir_result.ok()) {
    decimator_.Reset();
    fault_latched_ = true;
    CaptureDspFrontendResult result =
        MakeResult(CaptureDspFrontendCode::kTransformFault, continuity);
    result.mapper_saturated_samples = mapper_result.saturated_samples;
    result.fir_saturated_samples = fir_result.saturated_samples;
    return result;
  }

  output->metadata = input.metadata;
  CaptureDspFrontendResult result =
      MakeResult(CaptureDspFrontendCode::kProduced, continuity);
  result.mapper_saturated_samples = mapper_result.saturated_samples;
  result.fir_saturated_samples = fir_result.saturated_samples;
  return result;
}

}  // namespace boompi::audio
