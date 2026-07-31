#include <csignal>
#include <cstdlib>
#include <iostream>
#include <string_view>

#include "boompi/config/voice_client_config.h"
#include "boompi/voice/client.h"

namespace {
volatile std::sig_atomic_t g_stop = 0;
void Stop(int) { g_stop = 1; }
}

int main(int argc, char* argv[]) {
  const std::string_view mode = argc == 2 ? argv[1] : "--voice-loop";
  if (argc > 2 || (mode != "--voice-loop" && mode != "--check-config")) {
    std::cerr << "usage: boompi-client [--voice-loop|--check-config]\n"; return EXIT_FAILURE;
  }
  boompi::config::VoiceClientConfig config;
  const boompi::Status loaded = boompi::config::LoadVoiceClientConfigFromEnvironment(&config);
  if (!loaded.ok()) {
    std::cerr << "boompi-client: configuration failed: "
              << loaded.message() << '\n';
    return EXIT_FAILURE;
  }
  if (mode == "--check-config") {
    std::cout << "boompi-client: configuration is valid\n";
    return EXIT_SUCCESS;
  }
  std::signal(SIGINT, Stop); std::signal(SIGTERM, Stop);
  const boompi::Status result = boompi::voice::RunVoiceClient(config, &g_stop);
  if (!result.ok()) {
    std::cerr << "boompi-client: voice loop failed: " << result.message()
              << '\n';
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
