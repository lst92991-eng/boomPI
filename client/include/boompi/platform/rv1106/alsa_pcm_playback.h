#ifndef BOOMPI_PLATFORM_RV1106_ALSA_PCM_PLAYBACK_H_
#define BOOMPI_PLATFORM_RV1106_ALSA_PCM_PLAYBACK_H_

#include <cstdint>
#include <string>

#include "boompi/event/status.h"
#include "boompi/platform/audio_pcm_playback.h"

namespace boompi::platform::rv1106 {

struct AlsaPcmPlaybackConfig final {
  // Supplied by product configuration or a board probe. No card/device alias
  // is selected by this adapter.
  std::string device_name;
  PcmPlaybackDeviceConfig device{};
};

// RAII owner for one nonblocking ALSA playback handle. Open is a cold-path
// exact negotiation; QueryStatus/Write/Drop/Prepare are called only by the
// owning playback thread. Mixer controls are deliberately outside this class.
class AlsaPcmPlaybackDevice final : public PcmPlaybackDevice {
public:
  AlsaPcmPlaybackDevice() noexcept = default;
  ~AlsaPcmPlaybackDevice() noexcept override;
  AlsaPcmPlaybackDevice(const AlsaPcmPlaybackDevice &) = delete;
  AlsaPcmPlaybackDevice &operator=(const AlsaPcmPlaybackDevice &) = delete;
  AlsaPcmPlaybackDevice(AlsaPcmPlaybackDevice &&) = delete;
  AlsaPcmPlaybackDevice &operator=(AlsaPcmPlaybackDevice &&) = delete;

  static Status Open(const AlsaPcmPlaybackConfig &config,
                     AlsaPcmPlaybackDevice *output);

  bool is_open() const noexcept { return handle_ != nullptr; }
  PcmPlaybackDeviceConfig config() const noexcept override {
    return actual_config_;
  }
  std::uint64_t MonotonicNowUs() const noexcept override;
  PcmPlaybackDeviceStatusResult QueryStatus() noexcept override;
  PcmPlaybackDeviceWriteResult
  TryWriteInterleavedS16(const std::int16_t *samples,
                         std::uint32_t sample_frames) noexcept override;
  PcmPlaybackDeviceControlResult Drop() noexcept override;
  PcmPlaybackDeviceControlResult Prepare() noexcept override;

private:
  void Close() noexcept;

  void *handle_{nullptr};
  void *status_{nullptr};
  PcmPlaybackDeviceConfig actual_config_{};
};

} // namespace boompi::platform::rv1106

#endif // BOOMPI_PLATFORM_RV1106_ALSA_PCM_PLAYBACK_H_
