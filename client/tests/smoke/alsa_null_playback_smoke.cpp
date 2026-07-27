#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>

#include "boompi/audio/playback_frame.h"
#include "boompi/platform/audio_pcm_playback.h"
#include "boompi/platform/rv1106/alsa_pcm_playback.h"

namespace {

int Fail(const char *const operation) {
  std::cerr << "boomPI ALSA null accepted-only smoke failed: " << operation
            << '\n';
  return EXIT_FAILURE;
}

} // namespace

int main() {
  boompi::platform::rv1106::AlsaPcmPlaybackConfig config{};
  config.device_name = "null";
  config.device.sample_rate_hz = 48000U;
  config.device.channels = 2U;
  config.device.period_sample_frames = 960U;
  config.device.buffer_sample_frames = 3840U;
  config.device.start_threshold_sample_frames = 960U;
  config.device.avail_min_sample_frames = 960U;

  boompi::platform::rv1106::AlsaPcmPlaybackDevice device;
  const boompi::Status open_status =
      boompi::platform::rv1106::AlsaPcmPlaybackDevice::Open(config, &device);
  if (!open_status.ok()) {
    std::cerr << open_status.message() << '\n';
    return Fail("open/configure");
  }

  boompi::platform::PcmPlaybackSink48k sink;
  if (!boompi::platform::PcmPlaybackSink48k::Create(&device, &sink).ok()) {
    return Fail("create sink");
  }
  if (!sink.Prepare().succeeded()) {
    return Fail("prepare");
  }

  std::array<std::int16_t, boompi::audio::PlaybackPcmFrame48k::kFrameSamples>
      samples{};
  for (std::size_t index = 0U; index < samples.size(); ++index) {
    samples[index] = static_cast<std::int16_t>(
        static_cast<std::int32_t>(index % 257U) - 128);
  }
  const auto write = sink.TryWriteInterleavedS16(
      samples.data(), static_cast<std::uint16_t>(samples.size()));
  if (!write.valid() || !write.accepted() ||
      write.accepted_sample_frames != samples.size() ||
      write.timing_source != boompi::audio::PlaybackTimingSource::kAlsaStatus) {
    return Fail("one nonblocking accepted write");
  }

  if (!sink.Drop().succeeded() || !sink.Prepare().succeeded()) {
    return Fail("drop/prepare");
  }

  std::cout << "boomPI ALSA null accepted-only smoke passed; "
               "this does not prove hardware playback or audibility\n";
  return EXIT_SUCCESS;
}
