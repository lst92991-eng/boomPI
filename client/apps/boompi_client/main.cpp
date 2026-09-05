/**
 * @file main.cpp
 * @brief 板端客户端的进程入口，负责选择运行模式并建立 application 生命周期。
 *
 * 这里保持很薄：命令行和环境变量在进入业务层前完成校验，SIGINT/SIGTERM
 * 只转换成停止标志，音频、网络和 UI 资源统一交给 RunVoiceClient 管理。学生从
 * 本文件可以先看到程序的三种入口，再沿正常语音模式进入对话状态机。
 */
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <string_view>

#include "boompi/application/voice_client.h"
#include "boompi/config/voice_client_config.h"
#include "boompi/network/voice_link.h"

namespace {
// POSIX signal handler 运行在异步信号上下文，只能修改可安全访问的标量。
// application actor 每 20 ms 轮询该标志，并在正常线程上下文中依次关闭资源。
volatile std::sig_atomic_t g_stop = 0;
void Stop(int) { g_stop = 1; }
}

int main(int argc, char* argv[]) {
  // 无参数时进入产品语音循环；另外两个模式服务于安装和配网阶段，均可独立执行。
  const std::string_view mode = argc == 2 ? argv[1] : "--voice-loop";
  if (argc > 2 || (mode != "--voice-loop" && mode != "--check-config" &&
                   mode != "--save-wifi")) {
    std::cerr << "usage: boompi-client [--voice-loop|--check-config|--save-wifi]\n";
    return EXIT_FAILURE;
  }
  if (mode == "--save-wifi") {
    // SSID 与密码从标准输入分两行读取，避免密码出现在进程命令行和 shell 历史中。
    // 配网发生在语音配置加载之前，因此尚未配置服务端的设备也能保存 Wi-Fi。
    std::string ssid, password;
    if (!std::getline(std::cin, ssid) || !std::getline(std::cin, password) ||
        !boompi::network::SaveWifi(ssid, password)) return EXIT_FAILURE;
    return EXIT_SUCCESS;
  }
  boompi::config::VoiceClientConfig config;
  std::string error;
  // 所有可配置边界在打开 ALSA、启动线程和连接网络前一次性验证。
  // 后续实时路径只读取稳定配置，省去每帧解析和跨线程配置同步。
  if (!boompi::config::LoadVoiceClientConfigFromEnvironment(&config, &error)) {
    std::cerr << "boompi-client: configuration failed: " << error << '\n';
    return EXIT_FAILURE;
  }
  if (mode == "--check-config") {
    // 该模式只验证配置，不接触板端硬件，适合安装脚本在启动守护进程前快速失败。
    std::cout << "boompi-client: configuration is valid\n";
    return EXIT_SUCCESS;
  }
  // 两种常见停止请求共用同一条有序退出路径，析构顺序由 application 层掌控。
  std::signal(SIGINT, Stop);
  std::signal(SIGTERM, Stop);
#if defined(SIGPIPE)
  std::signal(SIGPIPE, SIG_IGN);
#endif
  if (!boompi::application::RunVoiceClient(config, &g_stop, &error)) {
    std::cerr << "boompi-client: voice loop failed: " << error << '\n';
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
