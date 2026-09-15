/**
 * @file camera_capture.cpp
 * @brief SC3336 外部转换管线和最新帧交接。
 *
 * DeviceUi 收到 CameraOn 后启动本对象；worker fork shell 承载 v4l2-ctl → ffmpeg，
 * 从 stdout 累积一帧 320x180 RGB565 后才发布。UI 变慢时允许覆盖未消费预览帧，
 * 这种取最新帧语义只服务本地视频预览，与不能静默丢失 PCM 的语音链路不同。
 * CameraOff/关闭 UI → Stop → 终止进程组 → worker 回收管线 → join → 清空帧槽。
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

/** @brief 解码 waitpid 状态，区分正常退出、信号终止和无法取得子进程状态。 */
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

/**
 * @brief 回收直接子进程；尚未退出时向其独立进程组发送终止信号，超时再升级。
 * waitpid 只观察组长。组长已退出时直接返回，不再向进程组发信号，因此返回成功
 * 不证明所有后代进程都已退出；调用者不能把它当成整个进程组的完成确认。
 */
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

/** @brief 使尚未消费的像素失效；即使工作线程同时交帧，也不会复制到半清空数组。 */
void CameraCapture::ClearFrame() noexcept {
  std::lock_guard<std::mutex> lock(frame_mutex_);
  frame_ready_ = false;
  frame_.fill(0U);
}

/** @brief 失败只发布诊断及 Error；CapturePreviewTask() 已取得的 fd 和进程仍在其退出路径回收。
 */
void CameraCapture::Fail(const char* const reason) noexcept {
  std::fprintf(stderr, "boompi-ui: camera %s\n", reason);
  ClearFrame();
  status_.store(CameraStatus::Error);
  ui_wake_.notify_one();
}

/** @brief UI worker 消费容量为一帧的槽位，复制完成后才清 ready，锁外绘图。 */
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

/** @brief 累计交给页面的帧数，与管线收到帧数比较来定位消费速度。 */

/** @brief 进入页面时重置统计和状态，异步启动采集；调用方必须避免重复启动活跃 worker。 */
void CameraCapture::Start() noexcept {
  if (worker_.joinable()) {
    worker_.join();
  }
  ClearFrame();
  stop_.store(false);
  status_.store(CameraStatus::Starting);
  ui_wake_.notify_one();
  try {
    worker_ = std::thread(&CameraCapture::CapturePreviewTask, this);
  } catch (...) {
    Fail("worker could not start");
  }
}

/**
 * @brief 离页时先请求停读，再终止子进程组，使管道唤醒并让 worker 回收直接子进程。
 *
 * 100 ms 后若 child_ 尚未被 worker 清除则升级 SIGKILL；join 后再清空共享帧，
 * 防止清空后仍有旧线程写回像素。尚未 fork 的启动阶段也由 stop_ 控制后续退出。
 */
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
  status_.store(CameraStatus::Stopped);
  ui_wake_.notify_one();
}

/**
 * @brief camera worker 按“创建管线 → 完整帧拼装 → 发布 → 退出清理”顺序运行。
 *
 * 首帧预留 2 s 给外部工具启动，此后每得到完整帧重新给 1 s 期限；收到部分字节
 * 不续期，避免管线只吐出残片却一直占用预览。错误上报 UI，当前实例不自动重启。
 */
void CameraCapture::CapturePreviewTask() noexcept {
  // 1. 启动摄像头转换管线，通过 stdout 接收固定大小的像素帧。
  int output[2]{};
  if (pipe(output) != 0) {
    Fail("pipe failed");
    return;
  }
  // stdout 是唯一像素通道，工具诊断转入 /run 下的独立日志，避免文本混进定长帧。
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

  // 此数组只有 camera worker 写；read 允许短读，used 达到完整帧字节数后才共享。
  Frame captured{};
  auto* bytes = reinterpret_cast<std::uint8_t*>(captured.data());
  std::size_t used = 0U;
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  const char* failure = nullptr;
  bool have_frame = false;

  while (!stop_.load()) {
    // 2. 等待像素数据；部分读取继续拼接，完整帧到达前不更新画面。
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

    // 3. 交付最新完整帧，通知显示任务。
    const auto frame_ready_at = std::chrono::steady_clock::now();
    {
      std::lock_guard<std::mutex> lock(frame_mutex_);
      // 预览只保留最新一帧，页面变慢时不会累积旧画面。
      frame_ = captured;
      frame_ready_ = true;
    }
    status_.store(CameraStatus::Live);
    ui_wake_.notify_one();
    used = 0U;
    have_frame = true;
    deadline = frame_ready_at + std::chrono::seconds(1);
  }

  // 4. 关闭管道、回收子进程；只有非主动退出才显示故障。
  close(output[0]);
  if (!stop_.load() && failure != nullptr) {
    Fail(failure);
  }
  const int exit_status = StopUiProcessGroup(child);
  if (!stop_.load()) {
    LogExit(exit_status);
  }
  child_.store(-1);
}

}  // namespace boompi::ui
