#include "boompi/audio/audio_dsp_engine.h"

#include <type_traits>

namespace boompi::audio {
namespace {

static_assert(std::is_trivial<AudioDspProcessResult>::value,
              "audio DSP process result must remain a POD value");
static_assert(std::is_standard_layout<AudioDspProcessResult>::value,
              "audio DSP process result must remain standard-layout");

constexpr AudioDspFeatureMask kKnownAudioDspFeatures =
    kBoompiV1AudioDspFeatures;

bool IsKnownReferenceSource(const AudioDspReferenceSource source) noexcept {
  switch (source) {
    case AudioDspReferenceSource::kHardwareCapture:
    case AudioDspReferenceSource::kSoftwarePlayback:
      return true;
    case AudioDspReferenceSource::kUnspecified:
      return false;
  }
  return false;
}

bool IsKnownReferenceLayout(const AudioDspReferenceLayout layout) noexcept {
  switch (layout) {
    case AudioDspReferenceLayout::kMonoLeft:
    case AudioDspReferenceLayout::kMonoRight:
    case AudioDspReferenceLayout::kStereo:
      return true;
    case AudioDspReferenceLayout::kUnspecified:
      return false;
  }
  return false;
}

}  // namespace

Status ValidateAudioDspEngineConfig(const AudioDspEngineConfig& config) {
  if (!IsKnownReferenceSource(config.reference_source)) {
    return Status::Error(StatusCode::kInvalidArgument,
                         "audio DSP reference source is invalid");
  }
  if (!IsKnownReferenceLayout(config.reference_layout)) {
    return Status::Error(StatusCode::kInvalidArgument,
                         "audio DSP reference layout is invalid");
  }
  if ((config.requested_features &
       static_cast<AudioDspFeatureMask>(~kKnownAudioDspFeatures)) != 0U) {
    return Status::Error(StatusCode::kInvalidArgument,
                         "audio DSP feature mask contains unknown bits");
  }
  if (config.requested_features != kBoompiV1AudioDspFeatures) {
    return Status::Error(
        StatusCode::kInvalidArgument,
        "audio DSP v1 requires AEC, noise suppression, beamforming, and AGC");
  }
  return Status::Ok();
}

Status UnavailableAudioDspEngine::Configure(
    const AudioDspEngineConfig& config) {
  static_cast<void>(config);
  return Status::Error(StatusCode::kNotSupported,
                       "audio DSP backend is unavailable");
}

Status UnavailableAudioDspEngine::Arm(const std::uint32_t epoch,
                                      const std::uint32_t stream_id) {
  static_cast<void>(epoch);
  static_cast<void>(stream_id);
  return Status::Error(StatusCode::kNotSupported,
                       "audio DSP backend is unavailable");
}

void UnavailableAudioDspEngine::Disarm() noexcept {}

AudioDspProcessResult UnavailableAudioDspEngine::Process(
    const DspCaptureFrame16k& input, Mono16kFrame* const output) noexcept {
  static_cast<void>(input);
  if (output == nullptr) {
    return {AudioDspProcessCode::kInvalidOutput};
  }
  output->ResetHeader();
  return {AudioDspProcessCode::kBackendUnavailable};
}

}  // namespace boompi::audio
