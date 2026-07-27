#ifndef BOOMPI_AUDIO_CAPTURE_DSP_FRONTEND_H_
#define BOOMPI_AUDIO_CAPTURE_DSP_FRONTEND_H_

#include <cstdint>

#include "boompi/audio/audio_dsp_types.h"
#include "boompi/audio/audio_frame.h"
#include "boompi/audio/capture_channel_mapper.h"
#include "boompi/audio/fir_decimator_48_to_16.h"
#include "boompi/audio/frame_continuity_gate.h"
#include "boompi/event/status.h"

namespace boompi::audio {

// Fixed, planar input for the future 16 kHz AudioDspEngine adapter. The plane
// order is CaptureRole declaration order: MIC-L, MIC-R, REF-L, REF-R.
struct DspCaptureFrame16k final {
  static constexpr std::uint32_t kSampleRateHz = 16000U;
  static constexpr std::uint16_t kFrameDurationMs = 20U;
  static constexpr std::uint8_t kChannelCount =
      static_cast<std::uint8_t>(kDspCaptureChannelCount);
  static constexpr std::uint32_t kSamplesPerChannel =
      static_cast<std::uint32_t>(kSamplesPer16k20ms);
  static constexpr SampleFormat kSampleFormat = SampleFormat::kPcmS16Le;

  AudioFrameMetadata metadata{};
  CapturePlanes16k planes{};
};

enum class CaptureDspFrontendCode : std::uint8_t {
  kProduced = 0,
  kNotConfigured,
  kInvalidOutput,
  kNotArmed,
  kEpochMismatch,
  kStreamMismatch,
  kContinuityFault,
  kTransformFault,
  kFaulted,
};

// A small value result keeps expected frame drops and faults out of Status's
// string-bearing control path. continuity is the exact gate result when the
// gate ran; it is kNotArmed for pre-gate configuration/output errors.
struct CaptureDspFrontendResult final {
  CaptureDspFrontendCode code;
  FrameContinuityResult continuity;
  std::uint32_t mapper_saturated_samples;
  std::uint32_t fir_saturated_samples;

  constexpr bool produced() const noexcept {
    return code == CaptureDspFrontendCode::kProduced;
  }

  constexpr bool requires_new_generation() const noexcept {
    return code == CaptureDspFrontendCode::kContinuityFault ||
           code == CaptureDspFrontendCode::kTransformFault ||
           code == CaptureDspFrontendCode::kFaulted;
  }
};

// One DSP worker owns an instance and serializes Create, Arm, Disarm, and
// Process. The class has no internal synchronization. Create and Arm are cold
// control-path operations; Process performs no allocation, locking, logging,
// file I/O, or network I/O.
class CaptureDspFrontend final {
 public:
  CaptureDspFrontend() noexcept = default;
  CaptureDspFrontend(const CaptureDspFrontend&) = delete;
  CaptureDspFrontend& operator=(const CaptureDspFrontend&) = delete;
  CaptureDspFrontend(CaptureDspFrontend&&) = delete;
  CaptureDspFrontend& operator=(CaptureDspFrontend&&) = delete;

  // Create configures a fresh instance before its worker starts. An instance
  // is configured at most once; use a new instance and generation to change
  // the channel map. Every failure leaves an existing instance untouched.
  static Status Create(const ChannelMap& channel_map,
                       CaptureDspFrontend* output);

  // A successful Arm selects a new generation and clears all FIR history and
  // latched frontend faults. The actor must issue a strictly increasing epoch
  // and never reuse an earlier (epoch, stream_id) pair; after epoch wrap it
  // drains and reconstructs the frontend. A rejected Arm changes no state.
  Status Arm(std::uint32_t epoch, std::uint32_t stream_id);
  void Disarm() noexcept;

  // input is borrowed only for this call. output remains caller-owned and is
  // valid only when code is kProduced; no pointer is retained. A null output
  // is rejected before the continuity gate advances, so the same frame may be
  // retried. Produced metadata is copied unchanged from input.
  CaptureDspFrontendResult Process(const CaptureFrame& input,
                                   DspCaptureFrame16k* output) noexcept;

 private:
  FrameContinuityGate continuity_gate_{};
  CaptureChannelMapper mapper_{};
  FirDecimator48To16 decimator_{};
  CapturePlanes48k mapped_48k_{};
  bool configured_{false};
  bool fault_latched_{false};
};

}  // namespace boompi::audio

#endif  // BOOMPI_AUDIO_CAPTURE_DSP_FRONTEND_H_
