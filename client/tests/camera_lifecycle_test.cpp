// 只替换exec边界；执行真实pipe/poll/fork/进程组/线程及整帧交付。
#include <signal.h>
#include <sys/types.h>
#include <unistd.h>
#include <cerrno>
#include <chrono>
#include <fstream>
#include <iostream>
#include <thread>
#include "../src/ui/camera_capture.h"

namespace {
int scenario = 0;
std::array<unsigned char, 320 * 180 * 2> image{};
bool alive(pid_t pid) {
  std::ifstream file("/proc/" + std::to_string(pid) + "/stat");
  std::string line;
  std::getline(file, line);
  const auto at = line.rfind(')');
  return at != std::string::npos && at + 2 < line.size() && line[at + 2] != 'Z';
}
}
extern "C" int __wrap_execl(const char*, const char*, ...) {
  if (scenario == 2) {
    for (;;) {
      pause();
    }
  }
  if (scenario == 1) {
    const auto ignored = write(STDOUT_FILENO, image.data(), 17);
    (void)ignored;
    _exit(0);
  }
  const auto descendant = fork();
  if (descendant == 0) {
    for (;;) {
      pause();
    }
  }
  const auto pid = static_cast<unsigned>(getpid());
  for (unsigned i = 0; i < 4; ++i) {
    image[i] = static_cast<unsigned char>(pid >> (8 * i));
    image[4 + i] = static_cast<unsigned char>(static_cast<unsigned>(descendant) >> (8 * i));
  }
  std::size_t at = 0;
  while (at < image.size()) {
    const auto remaining = image.size() - at;
    const auto count = write(STDOUT_FILENO, image.data() + at, remaining < 997 ? remaining : 997);
    if (count <= 0) {
      _exit(2);
    }
    at += static_cast<std::size_t>(count);
  }
  for (;;) {
    pause();
  }
}
int main() {
  namespace camera = boompi::ui::camera_capture;
  using Status = boompi::ui::CameraStatus;
  using Clock = std::chrono::steady_clock;
  boompi::ui::page::Image output;
  for (int round = 0; round < 3; ++round) {
    for (scenario = 0; scenario < 3; ++scenario) {
      if (!camera::open() || !camera::open()) {
        return 1;
      }
      Status status = Status::Starting;
      bool received = false;
      const auto deadline = Clock::now() + std::chrono::seconds(1);
      while (Clock::now() < deadline && scenario != 2) {
        received = camera::read(output, status);
        if (received || status == Status::Error) {
          break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
      }
      const auto before = Clock::now();
      camera::close();
      camera::close();
      if (Clock::now() - before > std::chrono::seconds(1) ||
          (scenario == 0 && (!received || status != Status::Live)) ||
          (scenario == 1 && (received || status != Status::Error))) {
        return 2;
      }
      if (scenario == 0) {
        const auto pid = static_cast<pid_t>(output[0] | (static_cast<unsigned>(output[1]) << 16));
        const auto child = static_cast<pid_t>(output[2] | (static_cast<unsigned>(output[3]) << 16));
        if (alive(pid) || alive(child)) {
          std::cerr << "camera process leaked\n";
          return 3;
        }
      }
      if (camera::read(output, status) || status != Status::Stopped) {
        return 4;
      }
    }
  }
  return 0;
}
