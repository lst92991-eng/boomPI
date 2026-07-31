#ifndef BOOMPI_CONFIG_VOICE_CLIENT_CONFIG_H_
#define BOOMPI_CONFIG_VOICE_CLIENT_CONFIG_H_

#include <cstdint>
#include <string>

#include "boompi/event/status.h"

namespace boompi::config {

struct VoiceClientConfig final {
  VoiceClientConfig() = default;
  ~VoiceClientConfig() noexcept;

  VoiceClientConfig(const VoiceClientConfig&) = delete;
  VoiceClientConfig& operator=(const VoiceClientConfig&) = delete;

  std::string server_ip, server_spki_sha256;
  std::uint16_t server_port{17806U};
  std::string device_id, device_token;
  std::string capture_pcm, playback_pcm;
  std::uint8_t volume_percent{60U};
  std::uint16_t speaker_gain_percent{100U};
  std::string snowboy_resource_path, snowboy_model_path;
  std::string snowboy_sensitivity{"0.5"};
  std::int8_t capture_left_polarity{1}, capture_right_polarity{1};
  bool barge_in_enabled{true};
};

// Loads the production voice-loop configuration. The device token remains
// owned by output and is securely cleared when output is destroyed.
Status LoadVoiceClientConfigFromEnvironment(VoiceClientConfig* output);

}  // namespace boompi::config

#endif  // BOOMPI_CONFIG_VOICE_CLIENT_CONFIG_H_
