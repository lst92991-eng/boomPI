/** @file device_ui.cpp
 * @brief UI 线程按顺序接收快照、更新页面并处理用户操作。
 *
 * DisplayTouch 管理板端屏幕/触摸，CameraCapture 管理预览管线。
 * LVGL 对象只在 UI worker 内创建、访问和销毁。
 */
#include "boompi/ui/device_ui.h"

#include <fcntl.h>
#include <lvgl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <new>
#include <string>
#include <thread>

#include "../platform/rv1106/display_touch.h"
#include "boompi/ui/lvgl_screen.h"
#include "camera_capture.h"

namespace boompi::ui {
namespace {

constexpr int kScreenWidth = 320, kScreenHeight = 240, kDrawRows = 32;
constexpr char kSettings[] = "/userdata/boompi/config/ui.settings";
constexpr char kSettingsTemporary[] = "/userdata/boompi/config/ui.settings.tmp";
constexpr char kProvision[] = "/usr/sbin/boompi-provision";
constexpr char kFont[] = "/userdata/boompi/fonts/NotoSansCJK-Regular.ttc";
constexpr char kFallbackFont[] = "/oem/usr/share/simsun_en.ttf";
/** @brief 完整写出控制命令或像素块，并把 EINTR 留在本层重试。 */
bool WriteAll(int fd, const void* source, std::size_t bytes) {
  auto* data = static_cast<const std::uint8_t*>(source);
  while (bytes != 0U) {
    const ssize_t count = write(fd, data, bytes);
    if (count < 0 && errno == EINTR) {
      continue;
    }
    if (count <= 0) {
      return false;
    }
    data += count;
    bytes -= static_cast<std::size_t>(count);
  }
  return true;
}

/**
 * @brief 以临时文件加 rename 的方式提交音量。
 *
 * 提交前先写完临时文件，避免正式配置留下半行数据。仅在滑块释放时保存，
 * 拖动预览不会频繁写闪存。
 */
bool SaveVolume(std::uint8_t percent) {
  const std::string text = std::to_string(percent) + "\n";
  const int fd =
      open(kSettingsTemporary, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
  bool ok = fd >= 0 && WriteAll(fd, text.data(), text.size());
  if (ok) {
    ok = fsync(fd) == 0;
  }
  if (fd >= 0 && close(fd) != 0) {
    ok = false;
  }
  if (ok) {
    ok = std::rename(kSettingsTemporary, kSettings) == 0;
  }
  if (!ok) {
    unlink(kSettingsTemporary);
  }
  return ok;
}

}  // namespace

struct DeviceUi::Impl final {
  using CameraFrame = CameraCapture::Frame;

  Impl() noexcept : camera(wake) {}

  platform::rv1106::DisplayTouch hardware;
  std::atomic<bool> stop{false};
  std::atomic<int> action{-1}, volume_change{-1};
  pid_t provision_pid{-1};
  std::mutex view_mutex;
  std::condition_variable wake;
  CameraCapture camera;
  CameraStatus shown_camera{CameraStatus::Stopped};
  std::chrono::steady_clock::time_point last_frame{};
  bool have_display_frame{false};
  CameraFrame display_frame{};
  bool start_done{false}, start_ok{false}, view_dirty{true};
  UiView view;
  std::thread ui_thread;
  LvglScreen screen;
  lv_disp_draw_buf_t draw_buffer{};
  lv_disp_drv_t display_driver{};
  lv_indev_drv_t input_driver{};
  lv_disp_t* display{nullptr};
  lv_indev_t* input{nullptr};
  std::array<lv_color_t, kScreenWidth * kDrawRows> draw_pixels{};

  /** @brief UI worker 原子取得最近一次完整 UiView，并清除 dirty 标志。 */
  bool TakeView(UiView* next) {
    std::lock_guard<std::mutex> lock(view_mutex);
    if (!view_dirty) {
      return false;
    }
    *next = view;
    view_dirty = false;
    return true;
  }

  void UpdateView(const UiView& value) {
    std::lock_guard<std::mutex> lock(view_mutex);
    view = value;
    view_dirty = true;
    wake.notify_one();
  }

  static void Flush(lv_disp_drv_t* driver, const lv_area_t* area, lv_color_t* pixels) {
    auto* self = static_cast<Impl*>(driver->user_data);
    if (!self->hardware.Flush(*area, pixels)) {
      self->stop.store(true);
      self->wake.notify_one();
    }
    // 失败也必须确认本次 flush，让 LVGL 能结束当前绘制并退出。
    lv_disp_flush_ready(driver);
  }

  static void ReadInput(lv_indev_drv_t* driver, lv_indev_data_t* data) {
    auto* self = static_cast<Impl*>(driver->user_data);
    self->hardware.ReadInput(data);
  }

  /**
   * @brief 从二维码页面启动 Wi-Fi 配网进程。
   *
   * 二维码只携带临时热点凭据；boompi-provision 负责 AP、DHCP/DNS 和凭据保存。
   * --no-panel 保证子进程不会与本进程争用 SPI/LVGL。setsid() 建立独立进程组，
   * UI 退出时能够统一终止脚本派生的服务。重复点击在现有 child 退出前会被忽略。
   */
  void StartProvisioning() {
    if (provision_pid > 0) {
      return;
    }
    if (access("/sys/class/net/wlan0", F_OK) != 0 || access(kProvision, X_OK) != 0) {
      screen.SetProvisionMessage("配网服务不可用", true);
      return;
    }
    const pid_t child = fork();
    if (child == 0) {
      if (setsid() < 0) {
        _exit(126);
      }
      execl(kProvision, "boompi-provision", "--no-panel", static_cast<char*>(nullptr));
      _exit(127);
    }
    if (child > 0) {
      provision_pid = child;
    } else {
      screen.SetProvisionMessage("配网服务启动失败", true);
    }
  }

  /**
   * @brief 把同步 LVGL 回调分流到 application 邮箱或 UI 所有的慢速操作。
   *
   * 唤醒、打断和实时音量通过原子值快速交还 application；配网、摄像头生命周期和
   * 音量持久化仍归 UI worker 管理。写配置只发生在 VolumeCommit，滑块移动期间的
   * VolumePreview 只改变播放 gain，减少闪存写放大。
   */
  static void HandleEvent(LvglScreen::Event event, std::uint8_t value, void* context) {
    auto* self = static_cast<Impl*>(context);
    if (event == LvglScreen::Event::kWake) {
      self->action.store(static_cast<int>(UiActionKind::Wake));
    } else if (event == LvglScreen::Event::kInterrupt) {
      self->action.store(static_cast<int>(UiActionKind::Interrupt));
    } else if (event == LvglScreen::Event::kProvision) {
      self->StartProvisioning();
    } else if (event == LvglScreen::Event::kCameraOn) {
      self->camera.Start();
    } else if (event == LvglScreen::Event::kCameraOff) {
      self->camera.Stop();
    } else {
      self->volume_change.store(value);
      if (event == LvglScreen::Event::kVolumeCommit && !SaveVolume(value)) {
        std::fprintf(stderr, "boompi-ui: volume setting save failed\n");
      }
    }
  }

  /** @brief 只收取已退出的配网进程结果，不阻塞页面等待配网。 */
  void PollProvisioning() {
    if (provision_pid > 0) {
      int status = 0;
      if (waitpid(provision_pid, &status, WNOHANG) == provision_pid) {
        const bool saved = WIFEXITED(status) && WEXITSTATUS(status) == 0;
        screen.SetProvisionMessage(saved ? "Wi-Fi 已保存，正在连接" : "配网服务启动失败",
                                   !saved);
        provision_pid = -1;
      }
    }
  }

  // 复制快照后已释放 view_mutex，绘制不会长时间阻塞 application 发布状态。
  void ShowLatestView() {
    UiView next;
    if (TakeView(&next)) {
      screen.SetState(next.state);
      screen.SetVolume(next.volume);
      screen.SetText(next.text.data(), {});
    }
  }

  void ShowLatestCamera(std::chrono::steady_clock::time_point now) {
    const auto status = camera.status();
    if (status != shown_camera) {
      shown_camera = status;
      if (status == CameraStatus::Starting) {
        last_frame = now;
        have_display_frame = false;
      }
      screen.SetCameraStatus(status);
    }
    const bool new_frame = camera.TakeFrame(&display_frame);
    if (new_frame && status == CameraStatus::Live) {
      // FPS 基于真正交给页面的相邻帧计算，表示用户看到的预览吞吐量。
      const auto frame_ms =
          std::chrono::duration_cast<std::chrono::milliseconds>(now - last_frame).count();
      const unsigned fps_tenths = have_display_frame && frame_ms > 0
                                      ? static_cast<unsigned>(10000 / frame_ms)
                                      : CameraCapture::kTargetFps * 10U;
      last_frame = now;
      have_display_frame = true;
      screen.SetCameraFrame(display_frame.data(), display_frame.size(), fps_tenths);
      camera.MarkDisplayed();
      // 摄像头帧到达时立即刷新，避免等待普通 30 ms UI 周期额外增加预览延迟。
      lv_refr_now(display);
    }
  }

  /**
   * @brief 在 UI worker 中推进 LVGL、页面快照、触摸和摄像头显示。
   *
   * nice(5) 让显示与摄像头工作在普通低优先级，避免和 SCHED_FIFO 音频线程竞争。
   * 循环依次回收配网进程、推进 LVGL tick、应用 application 快照、接收摄像头帧并
   * 执行 timer handler。condition_variable 最多等待 30 ms，使空闲时不忙轮询，
   * 新状态、摄像头帧或关闭请求可立即唤醒。所有 LVGL API 都留在本线程及其
   * 同步事件回调中。
   */
  void Run() {
    static_cast<void>(nice(5));
    lv_init();
    lv_disp_draw_buf_init(&draw_buffer, draw_pixels.data(), nullptr,
                          static_cast<std::uint32_t>(draw_pixels.size()));
    lv_disp_drv_init(&display_driver);
    display_driver.hor_res = kScreenWidth;
    display_driver.ver_res = kScreenHeight;
    display_driver.flush_cb = Flush;
    display_driver.draw_buf = &draw_buffer;
    display_driver.user_data = this;
    display = lv_disp_drv_register(&display_driver);
    lv_indev_drv_init(&input_driver);
    input_driver.type = LV_INDEV_TYPE_POINTER;
    input_driver.read_cb = ReadInput;
    input_driver.user_data = this;
    input = lv_indev_drv_register(&input_driver);
    // 字体也属于启动契约：缺少中文字形时停止 UI，避免课堂演示中出现方框或崩溃。
    const char* font = access(kFont, R_OK) == 0 ? kFont : kFallbackFont;
    const bool ready = display != nullptr && input != nullptr && access(font, R_OK) == 0 &&
                       screen.Create(font);
    if (ready) {
      screen.SetEventHandler(HandleEvent, this);
    }
    {
      std::lock_guard<std::mutex> lock(view_mutex);
      start_ok = ready;
      start_done = true;
    }
    wake.notify_one();
    if (!ready) {
      ReleaseUi();
      return;
    }
    auto tick = std::chrono::steady_clock::now();
    while (!stop.load()) {
      PollProvisioning();
      const auto now = std::chrono::steady_clock::now();
      const auto elapsed =
          std::chrono::duration_cast<std::chrono::milliseconds>(now - tick).count();
      tick = now;
      if (elapsed > 0) {
        lv_tick_inc(static_cast<std::uint32_t>(elapsed));
      }
      ShowLatestView();
      ShowLatestCamera(now);
      static_cast<void>(lv_timer_handler());
      std::unique_lock<std::mutex> lock(view_mutex);
      wake.wait_for(lock, std::chrono::milliseconds(30));
    }
    ReleaseUi();
  }

  // 正常退出和分配失败共用同一回收顺序，避免留下camera线程或配网进程。
  void ReleaseUi() {
    if (provision_pid > 0) {
      static_cast<void>(StopUiProcessGroup(provision_pid));
      provision_pid = -1;
    }
    camera.Stop();
    screen.Destroy();
    if (input != nullptr) {
      lv_indev_delete(input);
    }
    if (display != nullptr) {
      lv_disp_remove(display);
    }
    input = nullptr;
    display = nullptr;
  }
};

DeviceUi::~DeviceUi() noexcept {
  Close();
}

std::uint8_t DeviceUi::LoadVolume(std::uint8_t fallback) noexcept {
  unsigned saved = 0U;
  std::FILE* file = std::fopen(kSettings, "r");
  if (file == nullptr) {
    return std::min<std::uint8_t>(fallback, 100U);
  }
  const bool valid = std::fscanf(file, "%u", &saved) == 1 && saved <= 100U;
  std::fclose(file);
  return valid ? static_cast<std::uint8_t>(saved) : std::min<std::uint8_t>(fallback, 100U);
}

bool DeviceUi::Open() {
  Close();
  impl_ = new (std::nothrow) Impl;
  if (impl_ == nullptr) {
    return false;
  }
  if (!impl_->hardware.Open()) {
    Close();
    return false;
  }
  try {
    impl_->ui_thread = std::thread([state = impl_] {
      try {
        state->Run();
      } catch (...) {
        std::fprintf(stderr, "boompi-ui: display worker failed\n");
        state->stop.store(true);
        state->wake.notify_all();
        state->ReleaseUi();
      }
    });
  } catch (...) {
    Close();
    return false;
  }
  // Open() 等待 UI worker 完成字体和页面创建，调用方不会向半初始化对象发布状态。
  std::unique_lock<std::mutex> lock(impl_->view_mutex);
  const bool started = impl_->wake.wait_for(lock, std::chrono::seconds(2), [this] {
    return impl_->start_done;
  }) && impl_->start_ok;
  lock.unlock();
  if (!started) {
    Close();
  }
  return started;
}

void DeviceUi::Show(const UiView& view) noexcept {
  if (impl_ != nullptr) {
    impl_->UpdateView(view);
  }
}

bool DeviceUi::Poll(UiAction* result) noexcept {
  if (impl_ == nullptr || result == nullptr) {
    return false;
  }

  // exchange 同时完成读取与确认；连续输入只保留尚未消费的最近值。
  const int action = impl_->action.exchange(-1);
  if (action >= 0) {
    result->kind = static_cast<UiActionKind>(action);
    return true;
  }
  const int volume = impl_->volume_change.exchange(-1);
  if (volume < 0) {
    return false;
  }
  result->kind = UiActionKind::Volume;
  result->volume = static_cast<std::uint8_t>(volume);
  return true;
}

void DeviceUi::Close() noexcept {
  if (impl_ == nullptr) {
    return;
  }
  // UI worker 负责停止 camera/provision 并销毁 LVGL；join 后关闭硬件 fd 可避免回调悬空。
  impl_->stop.store(true);
  impl_->wake.notify_one();
  if (impl_->ui_thread.joinable()) {
    impl_->ui_thread.join();
  }
  impl_->hardware.Close();
  delete impl_;
  impl_ = nullptr;
}

}  // namespace boompi::ui
