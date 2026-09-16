// 客户端入口：处理维护命令，随后初始化、运行、退出。
#include <csignal>
#include <cstdlib>
#include <string_view>

#include "boompi/application/voice_client.h"
#include "boompi/config/voice_client_config.h"
#include "boompi/debug.h"

namespace {
volatile std::sig_atomic_t stop_requested = 0;
void RequestStop(int) {
  stop_requested = 1;
}
}  // namespace

int main(int argc, char* argv[]) {
  const std::string_view command = argc <= 1 ? "--voice-loop" : argv[1];
  if (argc > 2 || (command != "--voice-loop" && command != "--check-config")) {
    return EXIT_FAILURE;
  }
  boompi::config::VoiceClientConfig config;
  std::string error;
  if (!boompi::config::LoadClientConfig(&config, &error)) {
    boompi::debug::log.failure(error.c_str());
    return EXIT_FAILURE;
  }
  if (command == "--check-config") {
    return EXIT_SUCCESS;
  }
  std::signal(SIGINT, RequestStop);
  std::signal(SIGTERM, RequestStop);
  std::signal(SIGPIPE, SIG_IGN);
  bool succeeded = App_Init(config);
  while (!stop_requested && succeeded) {
    succeeded = App_Process();
  }
  // 正常退出和初始化失败共用回收路径。
  App_Close();
  if (!succeeded) {
    boompi::debug::log.failure(App_GetError());
  }
  return succeeded ? EXIT_SUCCESS : EXIT_FAILURE;
}
