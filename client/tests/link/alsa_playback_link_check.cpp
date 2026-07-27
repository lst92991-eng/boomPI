#include <cstdint>

#include "boompi/platform/audio_pcm_playback.h"
#include "boompi/platform/rv1106/alsa_pcm_playback.h"

// With no arguments this executable is inert. The runtime-dependent branch is
// retained so every adapter entry point and its ALSA/clock dependencies must be
// resolved by both native and cross linkers. It is a link check, not HIL.
int main(int argc, char **argv) {
  boompi::platform::rv1106::AlsaPcmPlaybackDevice device;
  if (argc != 2) {
    return device.is_open() ? 1 : 0;
  }

  boompi::platform::rv1106::AlsaPcmPlaybackConfig config{};
  config.device_name = argv[1];
  config.device = boompi::platform::PcmPlaybackDeviceConfig{
      48'000U, 960U, 3'840U, 960U, 960U, 2U};
  if (!boompi::platform::rv1106::AlsaPcmPlaybackDevice::Open(config, &device)
           .ok()) {
    return 2;
  }

  boompi::platform::PcmPlaybackSink48k sink;
  if (!boompi::platform::PcmPlaybackSink48k::Create(&device, &sink).ok()) {
    return 3;
  }

  const std::uint64_t now_us = device.MonotonicNowUs();
  const auto status = device.QueryStatus();
  const std::int16_t silence[2] = {0, 0};
  const auto write = device.TryWriteInterleavedS16(silence, 1U);
  const auto drop = device.Drop();
  const auto prepare = device.Prepare();

  return now_us != 0U &&
                 status.code !=
                     boompi::platform::PcmPlaybackDeviceCode::kUnset &&
                 write.code !=
                     boompi::platform::PcmPlaybackDeviceCode::kUnset &&
                 drop.code != boompi::platform::PcmPlaybackDeviceCode::kUnset &&
                 prepare.code != boompi::platform::PcmPlaybackDeviceCode::kUnset
             ? 0
             : 4;
}
