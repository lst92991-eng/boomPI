/**
 * @file camera_capture.cpp
 * @brief SC3336 外部转换管线和最新帧交接。
 */
#include "camera_capture.h"

#include <fcntl.h>
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>

namespace boompi::ui {
namespace {

constexpr std::size_t kFrameBytes =
    CameraCapture::kWidth * CameraCapture::kHeight * sizeof(std::uint16_t);
constexpr auto kReportPeriod = std::chrono::seconds(5);

// video14 不支持 VIDIOC_S_PARM。管线必须持续读取 25 FPS，再由 ffmpeg 丢帧，
// 否则 5 FPS 的页面消费速度会反向堵住摄像头驱动。
constexpr char kCameraCommand[] =
    "/usr/bin/v4l2-ctl -d /dev/video14 "
    "--set-fmt-video=width=576,height=324,pixelformat=NV12 "
    "--stream-mmap=4 --stream-to=- 2>/run/boompi-camera-v4l2.log | "
    "/usr/bin/ffmpeg -hide_banner -loglevel error -f rawvideo -pixel_format nv12 "
    "-video_size 576x324 -framerate 25 -i pipe:0 "
    "-vf 'fps=5,scale=320:180:flags=fast_bilinear' "
    "-pix_fmt rgb565le -f rawvideo pipe:1 2>/run/boompi-camera-ffmpeg.log";

unsigned FpsTenths(const std::uint64_t frames,
                   const std::chrono::milliseconds elapsed) noexcept {
  if (elapsed.count() <= 0) {
    return 0U;
  }
  return static_cast<unsigned>(frames * 10000U / static_cast<std::uint64_t>(elapsed.count()));
}

void LogStats(const std::uint64_t pipeline_frames, const std::uint64_t displayed_frames,
              const std::uint64_t dropped_frames,
              const std::chrono::milliseconds frame_span) noexcept {
  // 帧率按首尾帧之间的区间计算，不把进程启动时间算进稳定吞吐。
  const unsigned pipeline_fps =
      FpsTenths(pipeline_frames > 0U ? pipeline_frames - 1U : 0U, frame_span);
  const unsigned display_fps =
      FpsTenths(displayed_frames > 0U ? displayed_frames - 1U : 0U, frame_span);
  float load_one = 0.0F;
  std::FILE* load = std::fopen("/proc/loadavg", "r");
  const bool have_load = load != nullptr && std::fscanf(load, "%f", &load_one) == 1;
  if (load != nullptr) {
    std::fclose(load);
  }

  if (have_load) {
    std::fprintf(stderr,
                 "boompi-ui: camera target_fps=%u pipeline_fps=%u.%u "
                 "display_fps=%u.%u dropped=%llu load1=%.2f\n",
                 CameraCapture::kTargetFps, pipeline_fps / 10U, pipeline_fps % 10U,
                 display_fps / 10U, display_fps % 10U,
                 static_cast<unsigned long long>(dropped_frames), load_one);
    return;
  }
  std::fprintf(stderr,
               "boompi-ui: camera target_fps=%u pipeline_fps=%u.%u "
               "display_fps=%u.%u dropped=%llu load1=unavailable\n",
               CameraCapture::kTargetFps, pipeline_fps / 10U, pipeline_fps % 10U,
               display_fps / 10U, display_fps % 10U,
               static_cast<unsigned long long>(dropped_frames));
}

void LogExit(const int status) noexcept {
  if (status < 0) {
    std::fprintf(stderr, "boompi-ui: camera process status unavailable\n");
  } else if (WIFEXITED(status)) {
    std::fprintf(stderr, "boompi-ui: camera process exit=%d\n", WEXITSTATUS(status));
  } else if (WIFSIGNALED(status)) {
    std::fprintf(stderr, "boompi-ui: camera process signal=%d\n", WTERMSIG(status));
  }
}

}  // namespace

int StopUiProcessGroup(const pid_t child) noexcept {
  if (child <= 0) {
    return -1;
  }
  int status = 0;
  pid_t result = waitpid(child, &status, WNOHANG);
  if (result == child) {
    return status;
  }
  if (result < 0 && errno != EINTR) {
    return -1;
  }

  // 调用方已把child设为进程组长；向负PID发信号可同时停止shell管线成员。
  static_cast<void>(kill(-child, SIGTERM));
  for (int retry = 0; retry < 100; ++retry) {
    result = waitpid(child, &status, WNOHANG);
    if (result == child) {
      return status;
    }
    if (result < 0 && errno != EINTR) {
      return -1;
    }
    usleep(10000);
  }
  static_cast<void>(kill(-child, SIGKILL));
  do {
    result = waitpid(child, &status, 0);
  } while (result < 0 && errno == EINTR);
  return result == child ? status : -1;
}

CameraCapture::~CameraCapture() noexcept {
  Stop();
}

void CameraCapture::ClearFrame() noexcept {
  std::lock_guard<std::mutex> lock(frame_mutex_);
  frame_ready_ = false;
  frame_.fill(0U);
}

void CameraCapture::Fail(const char* const reason) noexcept {
  std::fprintf(stderr, "boompi-ui: camera %s\n", reason);
  ClearFrame();
  status_.store(Status::Error);
  ui_wake_.notify_one();
}

bool CameraCapture::TakeFrame(Frame* const output) noexcept {
  if (output == nullptr) {
    return false;
  }
  std::lock_guard<std::mutex> lock(frame_mutex_);
  if (!frame_ready_) {
    return false;
  }
  *output = frame_;
  frame_ready_ = false;
  return true;
}

void CameraCapture::MarkDisplayed() noexcept {
  displayed_frames_.fetch_add(1U);
}

void CameraCapture::Start() noexcept {
  if (worker_.joinable()) {
    worker_.join();
  }
  ClearFrame();
  displayed_frames_.store(0U);
  stop_.store(false);
  status_.store(Status::Starting);
  ui_wake_.notify_one();
  try {
    worker_ = std::thread(&CameraCapture::Run, this);
  } catch (...) {
    Fail("worker could not start");
  }
}

void CameraCapture::Stop() noexcept {
  stop_.store(true);
  const pid_t child = child_.load();
  if (child > 0) {
    static_cast<void>(kill(-child, SIGTERM));
    usleep(100000);
    if (child_.load() == child) {
      static_cast<void>(kill(-child, SIGKILL));
    }
  }
  if (worker_.joinable()) {
    worker_.join();
  }
  ClearFrame();
  status_.store(Status::Stopped);
  ui_wake_.notify_one();
}

void CameraCapture::Run() noexcept {
  int output[2]{};
  if (pipe(output) != 0) {
    Fail("pipe failed");
    return;
  }
  const pid_t child = fork();
  if (child == 0) {
    close(output[0]);
    setpgid(0, 0);
    if (dup2(output[1], STDOUT_FILENO) < 0) {
      _exit(126);
    }
    close(output[1]);
    execl("/bin/sh", "sh", "-c", kCameraCommand, static_cast<char*>(nullptr));
    _exit(127);
  }
  close(output[1]);
  if (child < 0) {
    close(output[0]);
    Fail("fork failed");
    return;
  }
  static_cast<void>(setpgid(child, child));
  child_.store(child);

  Frame captured{};
  auto* bytes = reinterpret_cast<std::uint8_t*>(captured.data());
  std::size_t used = 0U;
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  auto report_at = std::chrono::steady_clock::now() + kReportPeriod;
  auto first_frame_at = std::chrono::steady_clock::time_point{};
  auto last_frame_at = std::chrono::steady_clock::time_point{};
  std::uint64_t pipeline_frames = 0U;
  std::uint64_t dropped_frames = 0U;
  const char* failure = nullptr;
  bool have_frame = false;

  while (!stop_.load()) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
      failure = have_frame ? "frame timeout" : "first frame timeout";
      break;
    }
    int timeout_ms = static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count());
    timeout_ms = std::max(timeout_ms, 1);
    pollfd descriptor{output[0], POLLIN, 0};
    int ready = 0;
    do {
      ready = ::poll(&descriptor, 1U, timeout_ms);
    } while (ready < 0 && errno == EINTR && !stop_.load());
    if (stop_.load()) {
      break;
    }
    if (ready == 0) {
      failure = have_frame ? "frame timeout" : "first frame timeout";
      break;
    }
    if (ready < 0 || (descriptor.revents & POLLIN) == 0) {
      failure = "stream ended";
      break;
    }
    const ssize_t count = read(output[0], bytes + used, kFrameBytes - used);
    if (count < 0 && errno == EINTR) {
      continue;
    }
    if (count <= 0) {
      failure = "stream read failed";
      break;
    }
    used += static_cast<std::size_t>(count);
    if (used != kFrameBytes) {
      continue;
    }

    const auto frame_ready_at = std::chrono::steady_clock::now();
    ++pipeline_frames;
    if (pipeline_frames == 1U) {
      first_frame_at = frame_ready_at;
    }
    last_frame_at = frame_ready_at;
    {
      std::lock_guard<std::mutex> lock(frame_mutex_);
      if (frame_ready_) {
        ++dropped_frames;
      }
      frame_ = captured;
      frame_ready_ = true;
    }
    status_.store(Status::Live);
    ui_wake_.notify_one();
    used = 0U;
    have_frame = true;
    deadline = frame_ready_at + std::chrono::seconds(1);
    if (frame_ready_at >= report_at) {
      LogStats(pipeline_frames, displayed_frames_.load(), dropped_frames,
               std::chrono::duration_cast<std::chrono::milliseconds>(last_frame_at -
                                                                     first_frame_at));
      report_at = frame_ready_at + kReportPeriod;
    }
  }

  close(output[0]);
  if (!stop_.load() && failure != nullptr) {
    Fail(failure);
  }
  LogExit(StopUiProcessGroup(child));
  if (pipeline_frames != 0U) {
    LogStats(
        pipeline_frames, displayed_frames_.load(), dropped_frames,
        std::chrono::duration_cast<std::chrono::milliseconds>(last_frame_at - first_frame_at));
  }
  child_.store(-1);
}

}  // namespace boompi::ui
