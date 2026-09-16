// 客户端入口：读取配置，初始化，运行，退出。
#include <fcntl.h>
#include <sys/file.h>

#include <csignal>
#include <cstdlib>

#include "boompi/application/voice_client.h"
#include "boompi/config/voice_client_config.h"
#include "boompi/debug.h"

namespace {
volatile std::sig_atomic_t stop_requested = 0;
void RequestStop(int) {
  stop_requested = 1;
}
}  // namespace

int main() {
  // 锁由进程持有到退出，内核自动释放；重复启动不能重新初始化正在使用的硬件。
  const int instance = open("/run/boompi-client.lock", O_CREAT | O_RDWR | O_CLOEXEC, 0600);
  if (instance < 0) {
    boompi::debug::log.failure("cannot open instance lock");
    return EXIT_FAILURE;
  }
  if (flock(instance, LOCK_EX | LOCK_NB) < 0) {
    boompi::debug::log.failure("client is already running or instance lock failed");
    return EXIT_FAILURE;
  }
  boompi::config::VoiceClientConfig config;
  std::string error;
  if (!boompi::config::LoadClientConfig(&config, &error)) {
    boompi::debug::log.failure(error.c_str());
    return EXIT_FAILURE;
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
