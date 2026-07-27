#include "boompi/test/fake_audio_engines.h"

#include <cstddef>

namespace boompi::test {
namespace {

constexpr audio::WakeWordProcessResult kNoEventResult{
    audio::WakeWordDecision::kNoEvent, audio::WakeWordError::kNone,
    0U, 0U, false};

bool IsKnownWakeWordError(const audio::WakeWordError error) noexcept {
  switch (error) {
    case audio::WakeWordError::kNone:
    case audio::WakeWordError::kInvalidFormat:
    case audio::WakeWordError::kNotInitialized:
    case audio::WakeWordError::kBackendUnavailable:
    case audio::WakeWordError::kBackendRejectedAudio:
    case audio::WakeWordError::kBackendException:
    case audio::WakeWordError::kUnexpectedKeywordIndex:
    case audio::WakeWordError::kResetFailed:
      return true;
  }
  return false;
}

bool HasExactMono16kContract(const audio::Mono16kFrame& frame) noexcept {
  return frame.HasValidLength() && frame.format.sample_rate_hz == 16000U &&
         frame.format.frame_duration_ms == 20U &&
         frame.format.channels == 1U &&
         frame.format.sample_format == audio::SampleFormat::kPcmS16Le &&
         frame.samples_per_channel == 320U && frame.metadata.epoch != 0U &&
         frame.metadata.stream_id != 0U;
}

}  // namespace

Status FakeAudioDspEngine::Configure(
    const audio::AudioDspEngineConfig& config) {
  ++configure_count_;
  if (configured_) {
    return Status::Error(StatusCode::kFailedPrecondition,
                         "fake audio DSP is already configured");
  }

  const Status validation = audio::ValidateAudioDspEngineConfig(config);
  if (!validation.ok()) {
    return validation;
  }

  configured_ = true;
  return Status::Ok();
}

Status FakeAudioDspEngine::Arm(const std::uint32_t epoch,
                               const std::uint32_t stream_id) {
  ++arm_count_;
  if (!configured_) {
    return Status::Error(StatusCode::kFailedPrecondition,
                         "fake audio DSP is not configured");
  }
  if (epoch == 0U || stream_id == 0U) {
    return Status::Error(StatusCode::kInvalidArgument,
                         "fake audio DSP generation must be non-zero");
  }
  if (last_epoch_ != 0U && epoch <= last_epoch_) {
    return Status::Error(StatusCode::kFailedPrecondition,
                         "fake audio DSP epoch must strictly increase");
  }

  active_epoch_ = epoch;
  active_stream_id_ = stream_id;
  last_epoch_ = epoch;
  armed_ = true;
  faulted_ = false;
  ++reset_count_;
  return Status::Ok();
}

void FakeAudioDspEngine::Disarm() noexcept {
  ++disarm_count_;
  if (armed_ || faulted_) {
    ++reset_count_;
  }
  active_epoch_ = 0U;
  active_stream_id_ = 0U;
  armed_ = false;
  faulted_ = false;
}

audio::AudioDspProcessResult FakeAudioDspEngine::Process(
    const audio::DspCaptureFrame16k& input,
    audio::Mono16kFrame* const output) noexcept {
  if (output == nullptr) {
    return {audio::AudioDspProcessCode::kInvalidOutput};
  }
  output->ResetHeader();

  if (!configured_) {
    return {audio::AudioDspProcessCode::kNotConfigured};
  }
  if (!armed_) {
    return {audio::AudioDspProcessCode::kNotArmed};
  }
  if (input.metadata.epoch != active_epoch_) {
    return {audio::AudioDspProcessCode::kEpochMismatch};
  }
  if (input.metadata.stream_id != active_stream_id_) {
    return {audio::AudioDspProcessCode::kStreamMismatch};
  }
  if (input.metadata.discontinuity) {
    faulted_ = true;
    return {audio::AudioDspProcessCode::kDiscontinuity};
  }
  if (faulted_) {
    return {audio::AudioDspProcessCode::kFaulted};
  }

  ++backend_process_count_;
  if (fail_next_process_) {
    fail_next_process_ = false;
    faulted_ = true;
    return {audio::AudioDspProcessCode::kBackendFailure};
  }

  for (std::size_t sample = 0U;
       sample < audio::kSamplesPer16k20ms; ++sample) {
    output->samples[sample] = input.planes[0U][sample];
  }
  output->metadata = input.metadata;
  output->format = {16000U, 20U, 1U, audio::SampleFormat::kPcmS16Le};
  output->samples_per_channel =
      static_cast<std::uint32_t>(audio::kSamplesPer16k20ms);
  return {audio::AudioDspProcessCode::kProduced};
}

bool FakeWakeWordEngine::SetNextResult(
    const audio::WakeWordProcessResult& result) noexcept {
  if (!result.valid() || scripted_result_pending_) {
    return false;
  }
  scripted_result_ = result;
  scripted_result_pending_ = true;
  return true;
}

bool FakeWakeWordEngine::SetNextResetError(
    const audio::WakeWordError error) noexcept {
  if (!IsKnownWakeWordError(error) || error == audio::WakeWordError::kNone ||
      reset_error_pending_) {
    return false;
  }
  next_reset_error_ = error;
  reset_error_pending_ = true;
  return true;
}

audio::WakeWordResetResult FakeWakeWordEngine::Reset(
    const audio::WakeWordResetReason reason) noexcept {
  ++reset_count_;
  last_reset_reason_ = reason;
  has_last_reset_reason_ = true;
  if (reset_error_pending_) {
    const audio::WakeWordError error = next_reset_error_;
    next_reset_error_ = audio::WakeWordError::kNone;
    reset_error_pending_ = false;
    return {error, false};
  }
  return {audio::WakeWordError::kNone, true};
}

audio::WakeWordProcessResult FakeWakeWordEngine::Process(
    const audio::Mono16kFrame& frame) noexcept {
  ++process_count_;
  if (!HasExactMono16kContract(frame)) {
    return {audio::WakeWordDecision::kInvalidFrame,
            audio::WakeWordError::kInvalidFormat, 0U, 0U, false};
  }
  if (!scripted_result_pending_) {
    return kNoEventResult;
  }

  const audio::WakeWordProcessResult result = scripted_result_;
  scripted_result_ = kNoEventResult;
  scripted_result_pending_ = false;
  return result;
}

}  // namespace boompi::test
