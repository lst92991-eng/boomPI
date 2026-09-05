/**
 * @file main.cpp
 * @brief 板端进程入口，支持运行、自检和保存Wi-Fi配置。
 */
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <string_view>

#include "boompi/application/voice_client.h"
#include "boompi/config/voice_client_config.h"
#include "boompi/network/voice_link.h"

namespace {
// 信号处理函数只置标志。线程回收和设备关闭留给主循环，不能在信号上下文中执行。
volatile std::sig_atomic_t g_stop = 0;
void Stop(int) {
  g_stop = 1;
}
}  // namespace

int main(int argc, char* argv[]) {
  const std::string_view mode = argc == 2 ? argv[1] : "--voice-loop";
  if (argc > 2 ||
      (mode != "--voice-loop" && mode != "--check-config" && mode != "--save-wifi")) {
    std::cerr << "usage: boompi-client [--voice-loop|--check-config|--save-wifi]\n";
    return EXIT_FAILURE;
  }
  if (mode == "--save-wifi") {
    // SSID 与密码从标准输入分两行读取，避免密码出现在进程命令行和 shell 历史中。
    std::string ssid, password;
    if (!std::getline(std::cin, ssid) || !std::getline(std::cin, password) ||
        !boompi::network::SaveWifi(ssid, password)) {
      return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
  }
  boompi::config::VoiceClientConfig config;
  std::string error;
  // 配置不合法时，不启动线程或打开设备。
  if (!boompi::config::LoadVoiceClientConfigFromEnvironment(&config, &error)) {
    std::cerr << "boompi-client: configuration failed: " << error << '\n';
    return EXIT_FAILURE;
  }
  if (mode == "--check-config") {
    // 该模式只验证配置，不接触板端硬件，适合安装脚本在启动守护进程前快速失败。
    std::cout << "boompi-client: configuration is valid\n";
    return EXIT_SUCCESS;
  }
  std::signal(SIGINT, Stop);
  std::signal(SIGTERM, Stop);
  // 本程序只在Linux板端运行；断开的socket交给网络层处理，不让SIGPIPE结束进程。
  std::signal(SIGPIPE, SIG_IGN);
  if (!boompi::application::RunVoiceClient(config, &g_stop, &error)) {
    std::cerr << "boompi-client: voice loop failed: " << error << '\n';
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
