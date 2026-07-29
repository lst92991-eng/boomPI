#ifndef BOOMPI_PLATFORM_RV1106_ROCKCHIP_VOICE_DSP_H_
#define BOOMPI_PLATFORM_RV1106_ROCKCHIP_VOICE_DSP_H_

#include <array>
#include <cstddef>
#include <cstdint>

namespace boompi::platform::rv1106 {

constexpr std::size_t kRockchipVoiceFrameSamples16k = 320U;
using RockchipVoiceFrame16k =
    std::array<std::int16_t, kRockchipVoiceFrameSamples16k>;

enum class RockchipVoiceDspStatus : std::uint8_t {
  kOk = 0,
  kAlreadyOpen,
  kNotOpen,
  kInvalidArgument,
  kParameterAllocationFailed,
  kParameterInitializationFailed,
  kBackendInitializationFailed,
  kFifoOverflow,
  kFifoUnderflow,
  kBackendProcessFailed,
};

// Product-specific Rockchip voice path. One capture/DSP thread owns this
// object and calls Process with aligned 20 ms planes. The vendor input is the
// BSP-validated [mic-left, mic-right, playback-reference] layout.
//
// Rockchip consumes 256 samples per channel, so fixed storage bridges the
// 320-sample product frame without padding or dropping input. Open primes one
// 20 ms silent output frame; every successful Process then returns exactly
// 320 mono samples with that fixed startup latency. Process allocates nothing.
class RockchipVoiceDsp final {
 public:
  RockchipVoiceDsp() noexcept = default;
  ~RockchipVoiceDsp() noexcept;
  RockchipVoiceDsp(const RockchipVoiceDsp&) = delete;
  RockchipVoiceDsp& operator=(const RockchipVoiceDsp&) = delete;
  RockchipVoiceDsp(RockchipVoiceDsp&&) = delete;
  RockchipVoiceDsp& operator=(RockchipVoiceDsp&&) = delete;

  RockchipVoiceDspStatus Open() noexcept;
  void Close() noexcept;
  RockchipVoiceDspStatus Process(
      const RockchipVoiceFrame16k& mic_left,
      const RockchipVoiceFrame16k& mic_right,
      const RockchipVoiceFrame16k& playback_reference,
      RockchipVoiceFrame16k* output) noexcept;
  bool is_open() const noexcept { return handle_ != nullptr; }

 private:
  static constexpr std::size_t kVendorBlockSamples = 256U;
  static constexpr std::size_t kVendorInputChannels = 3U;
  static constexpr std::size_t kInputFifoFrames = 512U;
  static constexpr std::size_t kOutputFifoSamples = 640U;

  void ResetFifos(bool prime_output) noexcept;
  bool PushInput(const RockchipVoiceFrame16k& mic_left,
                 const RockchipVoiceFrame16k& mic_right,
                 const RockchipVoiceFrame16k& playback_reference) noexcept;
  bool ProcessVendorBlock() noexcept;
  bool PopOutput(RockchipVoiceFrame16k* output) noexcept;

  void* handle_{nullptr};
  void* parameters_{nullptr};
  std::array<std::int16_t, kInputFifoFrames * kVendorInputChannels>
      input_fifo_{};
  std::array<std::int16_t, kOutputFifoSamples> output_fifo_{};
  std::array<std::int16_t, kVendorBlockSamples * kVendorInputChannels>
      vendor_input_{};
  std::array<std::int16_t, kVendorBlockSamples> vendor_output_{};
  std::size_t input_head_{0U};
  std::size_t input_count_{0U};
  std::size_t output_head_{0U};
  std::size_t output_count_{0U};
};

}  // namespace boompi::platform::rv1106

#endif  // BOOMPI_PLATFORM_RV1106_ROCKCHIP_VOICE_DSP_H_
