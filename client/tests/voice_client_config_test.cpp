#include "boompi/config/voice_client_config.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace {

bool SetEnvironment(const char* name, const std::string& value) {
#ifdef _WIN32
  return _putenv_s(name, value.c_str()) == 0;
#else
  return setenv(name, value.c_str(), 1) == 0;
#endif
}

bool UnsetEnvironment(const char* name) {
#ifdef _WIN32
  return _putenv_s(name, "") == 0;
#else
  return unsetenv(name) == 0;
#endif
}

bool SetValidEnvironment() {
  const std::vector<std::pair<const char*, std::string>> values = {
      {"BOOMPI_SERVER_IP", "127.0.0.1"},
      {"BOOMPI_SERVER_SPKI_SHA256",
       "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA="},
      {"BOOMPI_DEVICE_ID", "00000000-0000-4000-8000-000000000001"},
      {"BOOMPI_VOLUME_PERCENT", "37"},
      {"BOOMPI_SNOWBOY_SENSITIVITY", "0.65"},
      {"BOOMPI_VAD_MIN_DBFS", "-40"},
      {"BOOMPI_BARGE_MIN_DBFS", "-30"},
      {"BOOMPI_CAPTURE_LEFT_POLARITY", "-1"},
      {"BOOMPI_CAPTURE_RIGHT_POLARITY", "1"},
      // 旧镜像中的这些变量会被忽略，不能覆盖当前 BSP 的固定事实。
      {"BOOMPI_CAPTURE_PCM", "ignored"},
      {"BOOMPI_PLAYBACK_PCM", "ignored"},
      {"BOOMPI_SPEAKER_GAIN_PERCENT", "999"},
      {"BOOMPI_BARGE_IN_ENABLED", "0"},
      {"BOOMPI_SNOWBOY_RESOURCE_FILE", "ignored"},
      {"BOOMPI_SNOWBOY_MODEL_FILE", "ignored"},
  };
  for (const auto& value : values) {
    if (!SetEnvironment(value.first, value.second)) return false;
  }
  return true;
}

bool FixedBoardValuesAreKept() {
  boompi::config::VoiceClientConfig config;
  std::string error;
  return boompi::config::LoadVoiceClientConfigFromEnvironment(&config, &error) &&
         config.server_ip == "127.0.0.1" && config.server_port == 17806U &&
         config.device_id == "00000000-0000-4000-8000-000000000001" &&
         config.volume_percent == 37U && config.snowboy_sensitivity == "0.65" &&
         config.vad_min_dbfs == -40.0F && config.barge_min_dbfs == -30.0F &&
         config.capture_left_polarity == -1 && config.capture_right_polarity == 1 &&
         config.capture_pcm == "hw:0,0" && config.playback_pcm == "hw:0,0" &&
         config.snowboy_resource_path == "/userdata/boompi/models/common.res" &&
         config.snowboy_model_path == "/userdata/boompi/models/snowboy.umdl" &&
         config.speaker_gain_percent == 100U && config.barge_in_enabled;
}

bool DefaultVadAdmissionIsMinus30Dbfs() {
  if (!SetValidEnvironment() || !UnsetEnvironment("BOOMPI_VAD_MIN_DBFS")) {
    return false;
  }
  boompi::config::VoiceClientConfig config;
  std::string error;
  return boompi::config::LoadVoiceClientConfigFromEnvironment(&config, &error) &&
         config.vad_min_dbfs == -30.0F;
}

bool Rejects(const char* name, const std::string& value) {
  if (!SetValidEnvironment() || !SetEnvironment(name, value)) return false;
  boompi::config::VoiceClientConfig config;
  std::string error;
  return !boompi::config::LoadVoiceClientConfigFromEnvironment(&config, &error) &&
         error.find(name) != std::string::npos;
}

}  // namespace

int main() {
  int failures = 0;
  if (!SetValidEnvironment() || !FixedBoardValuesAreKept()) {
    std::cerr << "valid configuration was rejected\n";
    ++failures;
  }
  if (!DefaultVadAdmissionIsMinus30Dbfs()) {
    std::cerr << "default VAD admission is not -30 dBFS\n";
    ++failures;
  }
  const std::vector<std::pair<const char*, std::string>> invalid = {
      {"BOOMPI_DEVICE_ID", "00000000-0000-0000-0000-000000000000"},
      {"BOOMPI_DEVICE_ID", "00000000-0000-4000-8000-00000000000A"},
      {"BOOMPI_SERVER_SPKI_SHA256",
       "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA?="},
      {"BOOMPI_SERVER_SPKI_SHA256",
       "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAB="},
      {"BOOMPI_BARGE_MIN_DBFS", "1"},
  };
  for (std::size_t index = 0; index < invalid.size(); ++index) {
    if (!Rejects(invalid[index].first, invalid[index].second)) {
      std::cerr << "invalid configuration case " << index << " was accepted\n";
      ++failures;
    }
  }
  return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
