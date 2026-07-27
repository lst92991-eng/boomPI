#ifndef BOOMPI_PLATFORM_RV1106_ROCKCHIP_3A_AUDIO_DSP_ADAPTER_H_
#define BOOMPI_PLATFORM_RV1106_ROCKCHIP_3A_AUDIO_DSP_ADAPTER_H_

#include <array>
#include <cstddef>
#include <cstdint>

#include "boompi/audio/audio_dsp_engine.h"
#include "boompi/audio/audio_dsp_frame_bridge.h"
#include "boompi/event/status.h"

namespace boompi::platform::rv1106 {

constexpr std::size_t kRockchip3aSourceChannels = 2U;
constexpr std::size_t kRockchip3aReferenceChannels = 2U;
constexpr std::size_t kRockchip3aInputChannels =
    kRockchip3aSourceChannels + kRockchip3aReferenceChannels;
constexpr std::size_t kRockchip3aInputSamples =
    audio::kDspBlockSamplesPerChannel16k * kRockchip3aInputChannels;
constexpr std::uint32_t kRockchip3aOutputBytes =
    static_cast<std::uint32_t>(audio::kDspBlockSamplesPerChannel16k *
                               sizeof(std::int16_t));

// The logical per-sample order is MIC-L, MIC-R, REF-L, REF-R. Physical ALSA
// slot validation and channel mapping remain upstream responsibilities.
using Rockchip3aInterleavedBlock16k =
    std::array<std::int16_t, kRockchip3aInputSamples>;

enum class Rockchip3aVendorInitCode : std::uint8_t {
  kUnset = 0,
  kReady,
  kAlreadyInitialized,
  kParameterAllocationFailed,
  kParameterInitializationFailed,
  kBackendInitializationFailed,
};

struct Rockchip3aVendorInitResult final {
  Rockchip3aVendorInitCode code;

  constexpr bool ready() const noexcept {
    return code == Rockchip3aVendorInitCode::kReady;
  }
};

enum class Rockchip3aVendorProcessCode : std::uint8_t {
  kUnset = 0,
  kProduced,
  kNotInitialized,
  kInvalidArgument,
  kBackendRejectedInput,
  kUnexpectedOutput,
};

struct Rockchip3aVendorProcessResult final {
  Rockchip3aVendorProcessCode code;
  std::uint32_t produced_bytes;

  constexpr bool produced_exact_block() const noexcept {
    return code == Rockchip3aVendorProcessCode::kProduced &&
           produced_bytes == kRockchip3aOutputBytes;
  }
};

// One DSP worker exclusively owns calls into a session. Initialize and
// Destroy are cold control-path operations. Process is an allocation-free
// hot-path call and must not retain either borrowed buffer.
class Rockchip3aVendorSession {
 public:
  virtual ~Rockchip3aVendorSession() noexcept = default;
  Rockchip3aVendorSession(const Rockchip3aVendorSession&) = delete;
  Rockchip3aVendorSession& operator=(const Rockchip3aVendorSession&) = delete;

  virtual Rockchip3aVendorInitResult Initialize() noexcept = 0;
  virtual Rockchip3aVendorProcessResult Process(
      const Rockchip3aInterleavedBlock16k& input,
      audio::DspMonoBlock16k16ms* output) noexcept = 0;
  virtual void Destroy() noexcept = 0;

 protected:
  Rockchip3aVendorSession() noexcept = default;
};

// Concrete wrapper around the pinned rkaudio preprocess ABI. Its definition
// is linked only by the explicit Debug feasibility build; default host and
// Release targets never include the vendor header or library.
class Rockchip3aPreprocessSession final : public Rockchip3aVendorSession {
 public:
  Rockchip3aPreprocessSession() noexcept = default;
  ~Rockchip3aPreprocessSession() noexcept override;
  Rockchip3aPreprocessSession(const Rockchip3aPreprocessSession&) = delete;
  Rockchip3aPreprocessSession& operator=(
      const Rockchip3aPreprocessSession&) = delete;

  Rockchip3aVendorInitResult Initialize() noexcept override;
  Rockchip3aVendorProcessResult Process(
      const Rockchip3aInterleavedBlock16k& input,
      audio::DspMonoBlock16k16ms* output) noexcept override;
  void Destroy() noexcept override;

 private:
  void* handle_{nullptr};
  void* parameters_{nullptr};
};

// RV1106 platform adapter for the fixed 16 ms vendor backend. It deliberately
// does not implement the frozen one-call/one-output AudioDspEngine contract.
// Instead, it exposes the existing frame-bridge result and pop semantics so a
// 20 ms input can be consumed without padding, dropping, or relabeling PCM.
//
// One DSP worker exclusively owns the adapter and externally-owned session.
// The session must outlive the adapter. Process/TryPop are allocation-free;
// Arm/Disarm recreate and destroy vendor history on the cold control path.
class Rockchip3aAudioDspAdapter final
    : private audio::AudioDspBlockProcessor16k {
 public:
  Rockchip3aAudioDspAdapter() noexcept = default;
  ~Rockchip3aAudioDspAdapter() noexcept;
  Rockchip3aAudioDspAdapter(const Rockchip3aAudioDspAdapter&) = delete;
  Rockchip3aAudioDspAdapter& operator=(
      const Rockchip3aAudioDspAdapter&) = delete;
  Rockchip3aAudioDspAdapter(Rockchip3aAudioDspAdapter&&) = delete;
  Rockchip3aAudioDspAdapter& operator=(Rockchip3aAudioDspAdapter&&) = delete;

  // This feasibility adapter accepts the complete v1 feature mask and two
  // reference planes only. It does not assert the vendor's internal algorithm
  // order or that product acoustic parameters have been approved.
  static Status Create(Rockchip3aVendorSession* session,
                       const audio::AudioDspEngineConfig& config,
                       Rockchip3aAudioDspAdapter* output);

  // Every successful Arm consumes a strictly newer epoch, flushes bridge
  // buffers, destroys any old vendor state, and initializes a fresh session.
  // Initialization failure disarms the bridge; recovery requires a newer
  // generation rather than retrying within the failed generation.
  Status Arm(std::uint32_t epoch, std::uint32_t stream_id);
  void Disarm() noexcept;

  audio::AudioDspFrameBridgeResult Process(
      const audio::DspCaptureFrame16k& input) noexcept;
  audio::AudioDspFrameBridgeResult TryPop(
      audio::Mono16kFrame* output) noexcept;

 private:
  audio::AudioDspBlockProcessResult Process(
      const audio::DspCapturePlanes16k16ms& input,
      audio::DspMonoBlock16k16ms* output) noexcept override;
  void DestroyVendor() noexcept;

  Rockchip3aVendorSession* session_{nullptr};
  audio::AudioDspFrameBridge16k bridge_{};
  Rockchip3aInterleavedBlock16k interleaved_input_{};
  bool configured_{false};
  bool vendor_ready_{false};
};

}  // namespace boompi::platform::rv1106

#endif  // BOOMPI_PLATFORM_RV1106_ROCKCHIP_3A_AUDIO_DSP_ADAPTER_H_
