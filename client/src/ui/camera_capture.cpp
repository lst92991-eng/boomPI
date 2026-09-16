#include "camera_capture.h"
#include <fcntl.h>
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <mutex>
#include <thread>

namespace boompi::ui::camera_capture {
namespace {
// video14不支持 VIDIOC_S_PARM；持续取25FPS，再由转换器降到5FPS，避免倒堵驱动。
constexpr char kCameraCommand[] =
    "/usr/bin/v4l2-ctl -d /dev/video14 "
    "--set-fmt-video=width=576,height=324,pixelformat=NV12 "
    "--stream-mmap=4 --stream-to=- 2>/run/boompi-camera-v4l2.log | "
    "/usr/bin/ffmpeg -hide_banner -loglevel error -f rawvideo -pixel_format nv12 "
    "-video_size 576x324 -framerate 25 -i pipe:0 "
    "-vf 'fps=5,scale=320:180:flags=fast_bilinear' "
    "-pix_fmt rgb565le -f rawvideo pipe:1 2>/run/boompi-camera-ffmpeg.log";
std::thread worker;
std::atomic<bool> stopping{false};
std::mutex mutex;
page::Image latest{}, captured{};
bool ready{false};
CameraStatus state{CameraStatus::Stopped};
void fail(const char* stage) {
  std::lock_guard<std::mutex> lock(mutex);
  ready = false;
  state = CameraStatus::Error;
  std::fprintf(stderr, "boompi-ui: camera %s\n", stage);
}
void capture() {
  int pipe_fds[2];
  if (pipe2(pipe_fds, O_CLOEXEC) < 0) {
    fail("pipe failed");
    return;
  }
  const auto child = fork();
  if (child == 0) {
    ::close(pipe_fds[0]);
    if (setpgid(0, 0) < 0 || dup2(pipe_fds[1], STDOUT_FILENO) < 0) {
      _exit(126);
    }
    ::close(pipe_fds[1]);
    execl("/bin/sh", "sh", "-c", kCameraCommand, static_cast<char*>(nullptr));
    _exit(127);
  }
  ::close(pipe_fds[1]);
  if (child < 0) {
    ::close(pipe_fds[0]);
    fail("fork failed");
    return;
  }
  // 父子两侧建立同一私有组；父侧失败不取消子侧检查。
  static_cast<void>(setpgid(child, child));
  using Clock = std::chrono::steady_clock;
  auto deadline = Clock::now() + std::chrono::seconds(2);
  auto* bytes = reinterpret_cast<unsigned char*>(captured.data());
  const auto frame_bytes = captured.size() * sizeof(captured[0]);
  std::size_t used = 0;
  while (!stopping.load()) {
    if (Clock::now() >= deadline) {
      fail("frame timeout");
      break;
    }
    // 固定短轮询使close可以退出；收到部分像素不续期。
    pollfd fd{pipe_fds[0], POLLIN, 0};
    const int available = poll(&fd, 1, 50);
    if (available == 0 || (available < 0 && errno == EINTR)) {
      continue;
    }
    if (available < 0 || !(fd.revents & POLLIN)) {
      fail("stream ended");
      break;
    }
    const auto count = ::read(pipe_fds[0], bytes + used, frame_bytes - used);
    if (count < 0 && errno == EINTR) {
      continue;
    }
    if (count <= 0) {
      fail("read failed");
      break;
    }
    used += static_cast<std::size_t>(count);
    if (used == frame_bytes) {
      std::lock_guard<std::mutex> lock(mutex);
      latest = captured;
      ready = true;
      state = CameraStatus::Live;
      used = 0;
      deadline = Clock::now() + std::chrono::seconds(1);
    }
  }
  // 仅本线程回收管线。组长在最后waitpid前不被回收，避免PID复用后误发信号。
  ::close(pipe_fds[0]);
  kill(-child, SIGTERM);
  usleep(50000);
  kill(-child, SIGKILL);
  int status;
  while (waitpid(child, &status, 0) < 0 && errno == EINTR) {
  }
}
}  // namespace
bool open() {
  if (worker.joinable()) {
    return true;
  }
  stopping.store(false);
  {
    std::lock_guard<std::mutex> lock(mutex);
    ready = false;
    state = CameraStatus::Starting;
  }
  try {
    worker = std::thread(capture);
    return true;
  } catch (...) {
    fail("thread failed");
    return false;
  }
}
bool read(page::Image& output, CameraStatus& status) {
  std::lock_guard<std::mutex> lock(mutex);
  status = state;
  if (!ready) {
    return false;
  }
  output = latest;
  ready = false;
  return true;
}
void close() noexcept {
  stopping.store(true);
  if (worker.joinable()) {
    worker.join();
  }
  std::lock_guard<std::mutex> lock(mutex);
  ready = false;
  state = CameraStatus::Stopped;
}
}  // namespace boompi::ui::camera_capture
