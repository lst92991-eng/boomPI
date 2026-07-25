#ifndef BOOMPI_CONFIG_CLIENT_CONFIG_H_
#define BOOMPI_CONFIG_CLIENT_CONFIG_H_

#include <cstddef>
#include <cstdint>

#include "boompi/event/status.h"

namespace boompi::config {

struct AudioConfig final {
  std::uint32_t capture_sample_rate_hz{48000};
  std::uint32_t dsp_sample_rate_hz{16000};
  std::uint32_t playback_sample_rate_hz{48000};
  std::uint16_t frame_duration_ms{20};
  std::uint8_t capture_channels{4};
  std::uint8_t default_volume_percent{60};
};

struct ClientConfig final {
  AudioConfig audio{};
  std::size_t event_queue_capacity{64};
  std::uint32_t follow_up_timeout_ms{3000};
  std::uint32_t initial_speech_timeout_ms{6000};
  std::uint32_t end_of_speech_ms{700};
};

ClientConfig DefaultClientConfig();
Status ValidateClientConfig(const ClientConfig& config);

}  // namespace boompi::config

#endif  // BOOMPI_CONFIG_CLIENT_CONFIG_H_
