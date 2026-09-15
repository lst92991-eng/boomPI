// 小智客户端入口：读取配置 → 初始化 → 运行 → 退出。
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <string_view>

#include "boompi/application/voice_client.h"
#include "boompi/config/voice_client_config.h"
#include "boompi/network/voice_link.h"

using boompi::config::LoadClientConfig;
using boompi::config::VoiceClientConfig;

namespace {
volatile std::sig_atomic_t stop_requested = 0;
bool IsVoiceMode(int argc, char* argv[]);
int RunCommand(int argc, char* argv[]);
void SetupExitSignals();
int ReportFailure(const std::string& message);
}  // namespace

int main(int argc, char* argv[]) {
  // 配置检查、Wi-Fi 保存等辅助命令，由文件下方处理。
  if (!IsVoiceMode(argc, argv)) {
    return RunCommand(argc, argv);
  }

  // 1. 读取配置。
  VoiceClientConfig config;
  std::string error;
  if (!LoadClientConfig(&config, &error)) {
    return ReportFailure(error);
  }

  // 2. 初始化显示、音频和网络。
  SetupExitSignals();
  bool succeeded = App_Init(config);

  // 3. 持续处理问答，直到退出信号或处理失败。
  while (stop_requested == 0 && succeeded) {
    succeeded = App_Process();
  }

  // 4. 关闭线程和设备，返回运行结果。
  App_Close();
  if (!succeeded) {
    return ReportFailure(App_GetError());
  }
  return EXIT_SUCCESS;
}

namespace {
// 不带参数和 --voice-loop 都进入上方的正常启动流程。
bool IsVoiceMode(int argc, char* argv[]) {
  return argc <= 1 || (argc == 2 && std::string_view(argv[1]) == "--voice-loop");
}

int RunCommand(int argc, char* argv[]) {
  const std::string_view command = argc == 2 ? argv[1] : "";
  if (command == "--check-config") {
    VoiceClientConfig config;
    std::string error;
    if (!LoadClientConfig(&config, &error)) {
      return ReportFailure(error);
    }
    std::cout << "boompi-client: configuration is valid\n";
    return EXIT_SUCCESS;
  }
  if (command == "--save-wifi") {
    // 凭据从标准输入读取，不放进命令行、历史或日志。
    std::string ssid;
    std::string password;
    if (!std::getline(std::cin, ssid) || !std::getline(std::cin, password) ||
        !boompi::network::SaveWifi(ssid, password)) {
      return ReportFailure("Wi-Fi configuration could not be saved");
    }
    return EXIT_SUCCESS;
  }
  return ReportFailure("usage: boompi-client [--voice-loop|--check-config|--save-wifi]");
}

// 信号只置位；资源由 main 中的正常退出流程回收。
void RequestStop(int) {
  stop_requested = 1;
}

void SetupExitSignals() {
  std::signal(SIGINT, RequestStop);
  std::signal(SIGTERM, RequestStop);
  std::signal(SIGPIPE, SIG_IGN);
}

int ReportFailure(const std::string& message) {
  std::cerr << "boompi-client: " << message << '\n';
  return EXIT_FAILURE;
}
}  // namespace
