#ifndef BOOMPI_PLATFORM_RV1106_ALSA_PCM_STREAM_H_
#define BOOMPI_PLATFORM_RV1106_ALSA_PCM_STREAM_H_

#include <cstdint>
#include <string>

#include "boompi/event/status.h"
#include "boompi/platform/audio_pcm_transfer.h"

namespace boompi::platform::rv1106 {

enum class AlsaPcmDirection : std::uint8_t {
  kCapture = 0,
  kPlayback,
};

struct AlsaPcmStreamConfig final {
  // Must be supplied by product configuration or a board probe. No card or
  // device number is selected by this adapter.
  std::string device_name;
  AlsaPcmDirection direction{AlsaPcmDirection::kCapture};
  std::uint32_t sample_rate_hz{48000U};
  std::uint8_t channels{0U};
  std::uint32_t period_sample_frames{960U};
  std::uint32_t buffer_sample_frames{3840U};
};

struct AlsaPcmActualConfig final {
  std::uint32_t sample_rate_hz{0U};
  std::uint8_t channels{0U};
  std::uint32_t period_sample_frames{0U};
  std::uint32_t buffer_sample_frames{0U};
};

// RAII owner for one nonblocking ALSA RW_INTERLEAVED S16_LE PCM handle. The
// class never changes mixer controls. Capture and playback each require their
// own instance and worker; no handle is shared across threads.
class AlsaPcmStream final : public PcmStreamBackend {
 public:
  AlsaPcmStream() noexcept = default;
  ~AlsaPcmStream() override;
  AlsaPcmStream(const AlsaPcmStream&) = delete;
  AlsaPcmStream& operator=(const AlsaPcmStream&) = delete;
  AlsaPcmStream(AlsaPcmStream&&) = delete;
  AlsaPcmStream& operator=(AlsaPcmStream&&) = delete;

  static Status Open(const AlsaPcmStreamConfig& config,
                     AlsaPcmStream* output);

  const AlsaPcmActualConfig& actual_config() const noexcept;
  bool is_open() const noexcept;

  PcmBackendResult Wait(std::uint32_t timeout_ms) noexcept override;
  PcmBackendResult ReadInterleaved(
      std::int16_t* samples,
      std::uint32_t requested_sample_frames) noexcept override;
  PcmBackendResult WriteInterleaved(
      const std::int16_t* samples,
      std::uint32_t requested_sample_frames) noexcept override;
  bool Recover(PcmBackendCode reason) noexcept override;
  bool Drop() noexcept override;
  bool Prepare() noexcept override;
  bool Reset() noexcept override;
  bool QueryQueuedSampleFrames(
      std::uint32_t* queued_sample_frames) noexcept override;

 private:
  void Close() noexcept;

  void* handle_{nullptr};
  AlsaPcmDirection direction_{AlsaPcmDirection::kCapture};
  AlsaPcmActualConfig actual_config_{};
};

}  // namespace boompi::platform::rv1106

#endif  // BOOMPI_PLATFORM_RV1106_ALSA_PCM_STREAM_H_
