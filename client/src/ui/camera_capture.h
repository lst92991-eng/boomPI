/**
 * @file camera_capture.h
 * @brief 为 UI 页面提供 SC3336 预览生命周期和容量为一帧的 RGB565 交接。
 *
 * LvglScreen 的 CameraOn → DeviceUi → Start() 启动管线；camera worker 持续拼出整帧，
 * UI worker TakeFrame() 后复制到页面，离页时 Stop() 终止子进程并等待线程结束。
 */
#pragma once

#include <sys/types.h>

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <thread>

#include "boompi/ui/ui_view.h"

namespace boompi::ui {

/**
 * @brief 请求终止由调用方建立的子进程组，并回收直接子进程。
 * @param child 必须是当前进程的子进程，同时也是目标进程组组长，不能传任意 PID。
 * @return waitpid 的原始状态，可用 WIFEXITED 等解析；无可用状态时返回 -1。
 *
 * 尚在运行时先 SIGTERM，轮询约 1 s 后升级为 SIGKILL 并回收。函数可能阻塞，
 * 只用于 UI 配网或 camera worker 清理，不能进入实时音频线程。
 */
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
  // ffmpeg 输出 rgb565le；本机帧以 16 位像素计数，不包含行间 padding 或视频头。
  using Frame = std::array<std::uint16_t, kWidth * kHeight>;

  /** @brief 借用 UI 唤醒条件变量，其生命周期必须覆盖本对象和 worker 退出。 */
  explicit CameraCapture(std::condition_variable& ui_wake) noexcept : ui_wake_(ui_wake) {}
  /** @brief 以 Stop() 完成进程和线程清理，不允许在自身 worker 中析构。 */
  ~CameraCapture() noexcept;
  CameraCapture(const CameraCapture&) = delete;
  CameraCapture& operator=(const CameraCapture&) = delete;

  /**
   * @brief UI worker 进入摄像头页时启动新采集，返回后通过 Status() 观察结果。
   *
   * 原有 worker 必须已停止或自然结束；函数会先 join 旧线程，不会先发停止请求。
   * 页面切换逻辑负责避免对正在采集的实例重复 Start()，线程创建失败发布 Error。
   */
  void Start() noexcept;
  /**
   * @brief UI worker 离页或退出时停止管线，join 后清除旧帧并发布 Stopped。
   *
   * 向进程组发信号可中断管道读取；允许重复调用，返回后本对象不再后台写帧。
   */
  void Stop() noexcept;
  /**
   * @brief 在短锁内复制最新完整帧并消费 ready 标记。
   * @param output 非空、由调用方持有的固定大小缓冲，成功后不依赖内部 frame_ 生命周期。
   * @return 有新帧为 true；没有新帧或指针为空为 false，输出保持原值。
   */
  bool TakeFrame(Frame* output) noexcept;
  /** @brief 原子读取管线阶段；状态与下一次 TakeFrame 是两次独立读取。 */
  CameraStatus Status() const noexcept {
    return status_.load();
  }

 private:
  /** @brief camera worker 拥有管道、子进程回收和整帧拼装，有限等待首帧及后续帧。 */
  void CapturePreviewTask() noexcept;
  /** @brief 记录具体采集阶段、丢弃未显示帧并唤醒 UI 显示 Error；不负责回收进程。 */
  void Fail(const char* reason) noexcept;
  /** @brief 持帧锁清空槽位，用于启动、停止或失败后阻止旧画面复用。 */
  void ClearFrame() noexcept;

  // 生命周期控制在 UI worker，stop_/child_/status_ 跨线程；frame_mutex_ 只保护帧及 ready。
  std::condition_variable& ui_wake_;
  std::atomic<bool> stop_{false};
  std::atomic<pid_t> child_{-1};
  std::atomic<CameraStatus> status_{CameraStatus::Stopped};
  std::mutex frame_mutex_;
  bool frame_ready_{false};
  Frame frame_{};
  std::thread worker_;
};

}  // namespace boompi::ui
