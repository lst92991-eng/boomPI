#include "boompi/platform/rv1106/rockchip_3a_audio_dsp_adapter.h"

#include <type_traits>

namespace boompi::platform::rv1106 {
namespace {

static_assert(std::is_trivial<Rockchip3aVendorInitResult>::value,
              "Rockchip 3A init result must remain a POD value");
static_assert(std::is_standard_layout<Rockchip3aVendorInitResult>::value,
              "Rockchip 3A init result must remain standard-layout");
static_assert(std::is_trivial<Rockchip3aVendorProcessResult>::value,
              "Rockchip 3A process result must remain a POD value");
static_assert(std::is_standard_layout<Rockchip3aVendorProcessResult>::value,
              "Rockchip 3A process result must remain standard-layout");

Status MapInitializationFailure(const Rockchip3aVendorInitCode code) {
  switch (code) {
    case Rockchip3aVendorInitCode::kParameterAllocationFailed:
      return Status::Error(StatusCode::kResourceExhausted,
                           "Rockchip 3A parameter allocation failed");
    case Rockchip3aVendorInitCode::kAlreadyInitialized:
    case Rockchip3aVendorInitCode::kParameterInitializationFailed:
    case Rockchip3aVendorInitCode::kBackendInitializationFailed:
      return Status::Error(StatusCode::kInternal,
                           "Rockchip 3A backend initialization failed");
    case Rockchip3aVendorInitCode::kUnset:
    case Rockchip3aVendorInitCode::kReady:
      break;
  }
  return Status::Error(StatusCode::kInternal,
                       "Rockchip 3A initialization result is invalid");
}

}  // namespace

Rockchip3aAudioDspAdapter::~Rockchip3aAudioDspAdapter() noexcept {
  Disarm();
}

Status Rockchip3aAudioDspAdapter::Create(
    Rockchip3aVendorSession* const session,
    const audio::AudioDspEngineConfig& config,
    Rockchip3aAudioDspAdapter* const output) {
  if (session == nullptr) {
    return Status::Error(StatusCode::kInvalidArgument,
                         "Rockchip 3A session must not be null");
  }
  if (output == nullptr) {
    return Status::Error(StatusCode::kInvalidArgument,
                         "Rockchip 3A adapter output must not be null");
  }
  if (output->configured_) {
    return Status::Error(StatusCode::kFailedPrecondition,
                         "Rockchip 3A adapter is already configured");
  }

  const Status config_status = audio::ValidateAudioDspEngineConfig(config);
  if (!config_status.ok()) {
    return config_status;
  }
  if (config.reference_layout != audio::AudioDspReferenceLayout::kStereo) {
    return Status::Error(
        StatusCode::kInvalidArgument,
        "Rockchip 3A feasibility adapter requires two reference planes");
  }

  const Status bridge_status =
      audio::AudioDspFrameBridge16k::Create(output, &output->bridge_);
  if (!bridge_status.ok()) {
    return bridge_status;
  }
  output->session_ = session;
  output->configured_ = true;
  return Status::Ok();
}

Status Rockchip3aAudioDspAdapter::Arm(const std::uint32_t epoch,
                                      const std::uint32_t stream_id) {
  if (!configured_) {
    return Status::Error(StatusCode::kFailedPrecondition,
                         "Rockchip 3A adapter is not configured");
  }

  const Status bridge_status = bridge_.Arm(epoch, stream_id);
  if (!bridge_status.ok()) {
    return bridge_status;
  }

  DestroyVendor();
  const Rockchip3aVendorInitResult init_result = session_->Initialize();
  if (!init_result.ready()) {
    session_->Destroy();
    bridge_.Disarm();
    return MapInitializationFailure(init_result.code);
  }
  vendor_ready_ = true;
  return Status::Ok();
}

void Rockchip3aAudioDspAdapter::Disarm() noexcept {
  bridge_.Disarm();
  DestroyVendor();
}

audio::AudioDspFrameBridgeResult Rockchip3aAudioDspAdapter::Process(
    const audio::DspCaptureFrame16k& input) noexcept {
  const audio::AudioDspFrameBridgeResult result = bridge_.Process(input);
  if (result.requires_new_generation()) {
    DestroyVendor();
  }
  return result;
}

audio::AudioDspFrameBridgeResult Rockchip3aAudioDspAdapter::TryPop(
    audio::Mono16kFrame* const output) noexcept {
  return bridge_.TryPop(output);
}

audio::AudioDspBlockProcessResult Rockchip3aAudioDspAdapter::Process(
    const audio::DspCapturePlanes16k16ms& input,
    audio::DspMonoBlock16k16ms* const output) noexcept {
  if (!vendor_ready_ || output == nullptr) {
    return {audio::AudioDspBlockProcessCode::kBackendFailure, 0U};
  }

  for (std::size_t sample = 0U;
       sample < audio::kDspBlockSamplesPerChannel16k; ++sample) {
    const std::size_t base = sample * kRockchip3aInputChannels;
    for (std::size_t channel = 0U; channel < kRockchip3aInputChannels;
         ++channel) {
      interleaved_input_[base + channel] = input[channel][sample];
    }
  }

  const Rockchip3aVendorProcessResult result =
      session_->Process(interleaved_input_, output);
  if (!result.produced_exact_block()) {
    return {audio::AudioDspBlockProcessCode::kBackendFailure, 0U};
  }
  return {audio::AudioDspBlockProcessCode::kProduced,
          static_cast<std::uint32_t>(
              audio::kDspBlockSamplesPerChannel16k)};
}

void Rockchip3aAudioDspAdapter::DestroyVendor() noexcept {
  if (session_ != nullptr && vendor_ready_) {
    session_->Destroy();
  }
  vendor_ready_ = false;
}

}  // namespace boompi::platform::rv1106
