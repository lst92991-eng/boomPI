#ifndef BOOMPI_AUDIO_AUDIO_DSP_ENGINE_H_
#define BOOMPI_AUDIO_AUDIO_DSP_ENGINE_H_

#include <cstdint>

#include "boompi/audio/audio_frame.h"
#include "boompi/audio/capture_dsp_frontend.h"
#include "boompi/event/status.h"

namespace boompi::audio {

enum class AudioDspReferenceSource : std::uint8_t {
  kUnspecified = 0,
  kHardwareCapture,
  kSoftwarePlayback,
};

enum class AudioDspReferenceLayout : std::uint8_t {
  kUnspecified = 0,
  kMonoLeft,
  kMonoRight,
  kStereo,
};

enum class AudioDspFeature : std::uint8_t {
  kEchoCancellation = 1U << 0U,
  kNoiseSuppression = 1U << 1U,
  kBeamforming = 1U << 2U,
  kAutomaticGainControl = 1U << 3U,
};

using AudioDspFeatureMask = std::uint8_t;

constexpr AudioDspFeatureMask ToMask(
    const AudioDspFeature feature) noexcept {
  return static_cast<AudioDspFeatureMask>(feature);
}

constexpr AudioDspFeatureMask kBoompiV1AudioDspFeatures =
    static_cast<AudioDspFeatureMask>(
        ToMask(AudioDspFeature::kEchoCancellation) |
        ToMask(AudioDspFeature::kNoiseSuppression) |
        ToMask(AudioDspFeature::kBeamforming) |
        ToMask(AudioDspFeature::kAutomaticGainControl));

struct AudioDspEngineConfig final {
  AudioDspReferenceSource reference_source{
      AudioDspReferenceSource::kUnspecified};
  AudioDspReferenceLayout reference_layout{
      AudioDspReferenceLayout::kUnspecified};
  AudioDspFeatureMask requested_features{0U};
};

// Control-path validation. The v1 engine requires exactly one explicit
// reference source/layout and the complete AEC/NS/BF/AGC feature set.
Status ValidateAudioDspEngineConfig(const AudioDspEngineConfig& config);

enum class AudioDspProcessCode : std::uint8_t {
  kProduced = 0,
  kBackendUnavailable,
  kNotConfigured,
  kNotArmed,
  kInvalidOutput,
  kInvalidInput,
  kEpochMismatch,
  kStreamMismatch,
  kDiscontinuity,
  kBackendFailure,
  kFaulted,
};

struct AudioDspProcessResult final {
  AudioDspProcessCode code;

  constexpr bool produced() const noexcept {
    return code == AudioDspProcessCode::kProduced;
  }

  constexpr bool requires_new_generation() const noexcept {
    return code == AudioDspProcessCode::kDiscontinuity ||
           code == AudioDspProcessCode::kBackendFailure ||
           code == AudioDspProcessCode::kFaulted;
  }
};

// One DSP worker exclusively owns an engine and serializes Configure, Arm,
// Disarm, Process, and destruction. Configure and Arm are cold control-path
// operations. Process is an allocation-free hot-path operation: it must not
// lock, log, perform I/O, or retain input/output pointers after returning.
//
// CaptureDspFrontend owns sequence/timestamp continuity checking. The engine
// still rejects wrong generations and explicit discontinuities before calling
// its backend. A produced output contains exactly one 16 kHz, 20 ms mono frame
// and copies the input metadata. On every non-produced result, a non-null
// output has an invalid header and its PCM storage must not be consumed.
class AudioDspEngine {
 public:
  virtual ~AudioDspEngine() = default;

  AudioDspEngine(const AudioDspEngine&) = delete;
  AudioDspEngine& operator=(const AudioDspEngine&) = delete;
  AudioDspEngine(AudioDspEngine&&) = delete;
  AudioDspEngine& operator=(AudioDspEngine&&) = delete;

  virtual Status Configure(const AudioDspEngineConfig& config) = 0;

  // Arm requires a non-zero epoch strictly greater than every epoch accepted
  // by this instance, plus a non-zero stream_id, and resets all backend
  // history. After epoch exhaustion the worker reconstructs the engine. A
  // rejected Arm must preserve the existing state.
  virtual Status Arm(std::uint32_t epoch, std::uint32_t stream_id) = 0;

  // Disarm is idempotent and invalidates all backend history.
  virtual void Disarm() noexcept = 0;

  virtual AudioDspProcessResult Process(
      const DspCaptureFrame16k& input,
      Mono16kFrame* output) noexcept = 0;

 protected:
  AudioDspEngine() = default;
};

// Fail-closed default for builds without a verified DSP backend. It never
// treats unprocessed microphone data as valid output.
class UnavailableAudioDspEngine final : public AudioDspEngine {
 public:
  Status Configure(const AudioDspEngineConfig& config) override;
  Status Arm(std::uint32_t epoch, std::uint32_t stream_id) override;
  void Disarm() noexcept override;
  AudioDspProcessResult Process(const DspCaptureFrame16k& input,
                                Mono16kFrame* output) noexcept override;
};

}  // namespace boompi::audio

#endif  // BOOMPI_AUDIO_AUDIO_DSP_ENGINE_H_
