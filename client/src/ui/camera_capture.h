#pragma once

#include <sys/types.h>

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <thread>

namespace boompi::ui {

/** 终止由调用方建立的子进程组，并回收组长进程。 */
int StopUiProcessGroup(pid_t child) noexcept;

/**
 * @brief 管理 SC3336 预览进程、采集线程和最新 RGB565 帧。
 *
 * UI worker 调用 Start/Stop/TakeFrame；内部已有的 camera worker 负责阻塞读取。
 * 只保留最新完整帧，避免页面变慢时累积旧画面。
 */
class CameraCapture final {
 public:
  static constexpr std::size_t kWidth = 320U;
  static constexpr std::size_t kHeight = 180U;
  static constexpr unsigned kTargetFps = 5U;
  using Frame = std::array<std::uint16_t, kWidth * kHeight>;
  enum class Status : std::uint8_t { Stopped, Starting, Live, Error };

  explicit CameraCapture(std::condition_variable& ui_wake) noexcept : ui_wake_(ui_wake) {}
  ~CameraCapture() noexcept;
  CameraCapture(const CameraCapture&) = delete;
  CameraCapture& operator=(const CameraCapture&) = delete;

  void Start() noexcept;
  void Stop() noexcept;
  bool TakeFrame(Frame* output) noexcept;
  void MarkDisplayed() noexcept;
  Status status() const noexcept {
    return status_.load();
  }

 private:
  void Run() noexcept;
  void Fail(const char* reason) noexcept;
  void ClearFrame() noexcept;

  std::condition_variable& ui_wake_;
  std::atomic<bool> stop_{false};
  std::atomic<pid_t> child_{-1};
  std::atomic<std::uint64_t> displayed_frames_{0U};
  std::atomic<Status> status_{Status::Stopped};
  std::mutex frame_mutex_;
  bool frame_ready_{false};
  Frame frame_{};
  std::thread worker_;
};

}  // namespace boompi::ui
