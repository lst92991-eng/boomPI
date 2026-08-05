/** @file 进程组合根：只负责参数、环境配置、信号和 application 生命周期。 */
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <string_view>

#include "boompi/application/voice_client.h"
#include "boompi/config/voice_client_config.h"
#include "boompi/network/network_bootstrap.h"

namespace {
volatile std::sig_atomic_t g_stop = 0;
// 信号处理函数只能写 sig_atomic_t；资源回收仍由正常控制流执行。
void Stop(int) { g_stop = 1; }
}

int main(int argc, char* argv[]) {
  const std::string_view mode = argc == 2 ? argv[1] : "--voice-loop";
  if (argc > 2 || (mode != "--voice-loop" && mode != "--check-config" &&
                   mode != "--save-wifi")) {
    std::cerr << "usage: boompi-client [--voice-loop|--check-config|--save-wifi]\n";
    return EXIT_FAILURE;
  }
  if (mode == "--save-wifi") {
    std::string ssid, password;
    if (!std::getline(std::cin, ssid) || !std::getline(std::cin, password) ||
        !boompi::network::NetworkBootstrap::SaveWifi(ssid, password)) return EXIT_FAILURE;
    return EXIT_SUCCESS;
  }
  boompi::config::VoiceClientConfig config;
  std::string error;
  if (!boompi::config::LoadVoiceClientConfigFromEnvironment(&config, &error)) {
    std::cerr << "boompi-client: configuration failed: " << error << '\n';
    return EXIT_FAILURE;
  }
  if (mode == "--check-config") {
    std::cout << "boompi-client: configuration is valid\n";
    return EXIT_SUCCESS;
  }
  std::signal(SIGINT, Stop); std::signal(SIGTERM, Stop);
  if (!boompi::application::RunVoiceClient(config, &g_stop, &error)) {
    std::cerr << "boompi-client: voice loop failed: " << error << '\n';
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
