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

bool SetValidEnvironment() {
  const std::vector<std::pair<const char*, std::string>> values = {
      {"BOOMPI_SERVER_IP", "127.0.0.1"},
      {"BOOMPI_SERVER_SPKI_SHA256",
       "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA="},
      {"BOOMPI_DEVICE_ID", "00000000-0000-4000-8000-000000000001"},
      {"BOOMPI_DEVICE_TOKEN", "boompi-valid-token-0123456789abcdef"},
      {"BOOMPI_CAPTURE_PCM", "hw:0,0"},
      {"BOOMPI_PLAYBACK_PCM", "hw:0,0"},
      {"BOOMPI_SNOWBOY_RESOURCE_FILE", "common.res"},
      {"BOOMPI_SNOWBOY_MODEL_FILE", "model.pmdl"},
  };
  for (const auto& value : values) {
    if (!SetEnvironment(value.first, value.second)) return false;
  }
  return true;
}

bool LoadSucceeds() {
  boompi::config::VoiceClientConfig config;
  std::string error;
  return boompi::config::LoadVoiceClientConfigFromEnvironment(&config, &error);
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
  if (!SetValidEnvironment() || !LoadSucceeds()) {
    std::cerr << "valid configuration was rejected\n";
    ++failures;
  }
  const std::vector<std::pair<const char*, std::string>> invalid = {
      {"BOOMPI_DEVICE_ID", "00000000-0000-0000-0000-000000000000"},
      {"BOOMPI_DEVICE_ID", "00000000-0000-4000-8000-00000000000A"},
      {"BOOMPI_SERVER_SPKI_SHA256",
       "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA?="},
      {"BOOMPI_SERVER_SPKI_SHA256",
       "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAB="},
      {"BOOMPI_DEVICE_TOKEN", std::string(31U, 'x') + " "},
      {"BOOMPI_DEVICE_TOKEN", std::string(31U, 'x') + '\x7f'},
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
