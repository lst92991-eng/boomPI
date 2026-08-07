/**
 * @file device_ui.cpp
 * @brief 把 RV1106 显示、触摸、摄像头与持久化配置接到单线程 LVGL 页面。
 *
 * application 线程只交换状态快照和单槽事件；UI worker 独占 LVGL、SPI 和
 * GT911 轮询；camera worker 独占外部采集管线。文件中的同步点用于跨越这三种
 * 所有权边界，任何慢速 I/O 都不会进入 ALSA capture/playback 热路径。
 */
#include "boompi/ui/device_ui.h"

#include <fcntl.h>
#include <linux/i2c.h>
#include <linux/i2c-dev.h>
#include <linux/spi/spidev.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <new>
#include <string>
#include <thread>

#include <lvgl.h>

#include "boompi/ui/lvgl_screen.h"

namespace boompi::ui {
namespace {

// 面板原生方向为 240x320，产品界面按 320x240 横屏绘制；刷屏和触摸都在端口层旋转。
constexpr int kPanelWidth = 240, kPanelHeight = 320;
constexpr int kScreenWidth = 320, kScreenHeight = 240, kDrawRows = 32;
// GPIO 编号来自当前 RV1106 板级连线，修改硬件版本时需要与 DTS 和网表一同核对。
constexpr int kBacklightGpio = 53, kDataCommandGpio = 66, kPanelResetGpio = 67;
constexpr int kTouchResetGpio = 64, kTouchInterruptGpio = 73;
constexpr int kGt911Address = 0x14;
constexpr std::uint16_t kTouchStatus = 0x814E;
constexpr std::uint16_t kTouchPoint = 0x814F;
constexpr std::size_t kCameraPixels = 320U * 180U;
constexpr std::size_t kCameraBytes = kCameraPixels * sizeof(std::uint16_t);
constexpr unsigned kCameraFps = 5U;
constexpr auto kCameraReportPeriod = std::chrono::seconds(5);
constexpr unsigned kTouchFailureLimit = 3U;
constexpr unsigned kTouchRecoveryLimit = 2U;
// spidev 接收的是上限请求，Open() 还会回读驱动实际接受的频率并记录到日志。
constexpr unsigned kSpiSpeedHz = 80000000U;
constexpr char kGpioRoot[] = "/sys/class/gpio";
constexpr char kSettings[] = "/userdata/boompi/config/ui.settings";
constexpr char kSettingsTemporary[] = "/userdata/boompi/config/ui.settings.tmp";
constexpr char kProvision[] = "/usr/sbin/boompi-provision";
constexpr char kFont[] = "/userdata/boompi/fonts/NotoSansCJK-Regular.ttc";
constexpr char kFallbackFont[] = "/oem/usr/share/simsun_en.ttf";
// SC3336 先由 v4l2-ctl 输出 NV12，再由 ffmpeg 缩放并转换为页面直接消费的 RGB565。
// video14 不支持 VIDIOC_S_PARM，不能用 --set-parm 限帧。FFmpeg 持续读取 25 FPS，
// 先丢到 5 FPS 再缩放，既不让慢管道堵住驱动，也把页面负载固定在 5 FPS。
// 两个进程放在同一进程组，离开页面时整体终止。
constexpr char kCameraCommand[] =
    "/usr/bin/v4l2-ctl -d /dev/video14 "
    "--set-fmt-video=width=576,height=324,pixelformat=NV12 "
    "--stream-mmap=4 --stream-to=- 2>/run/boompi-camera-v4l2.log | "
    "/usr/bin/ffmpeg -hide_banner -loglevel error -f rawvideo -pixel_format nv12 "
    "-video_size 576x324 -framerate 25 -i pipe:0 "
    "-vf 'fps=5,scale=320:180:flags=fast_bilinear' "
    "-pix_fmt rgb565le -f rawvideo pipe:1 2>/run/boompi-camera-ffmpeg.log";

/** @brief 完整写出控制命令或像素块，并把 EINTR 留在本层重试。 */
bool WriteAll(int fd, const void* source, std::size_t bytes) {
  auto* data = static_cast<const std::uint8_t*>(source);
  while (bytes != 0U) {
    const ssize_t count = write(fd, data, bytes);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) return false;
    data += count;
    bytes -= static_cast<std::size_t>(count);
  }
  return true;
}

bool WriteText(const std::string& path, const std::string& text, int flags = O_WRONLY) {
  const int fd = open(path.c_str(), flags | O_CLOEXEC, 0644);
  if (fd < 0) return false;
  const bool ok = WriteAll(fd, text.data(), text.size());
  close(fd);
  return ok;
}

/**
 * @brief 以临时文件加 rename 的方式提交音量。
 *
 * 掉电或进程被杀时，旧配置保持完整；临时文件权限也避免课堂局域网中的普通用户
 * 读取或篡改设备设置。调用点只发生在滑块释放事件，拖动过程不会频繁写闪存。
 */
bool SaveVolume(std::uint8_t percent) {
  const std::string text = std::to_string(percent) + "\n";
  const int fd = open(kSettingsTemporary,
                      O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
  bool ok = fd >= 0 && WriteAll(fd, text.data(), text.size());
  if (fd >= 0 && close(fd) != 0) ok = false;
  if (ok) ok = std::rename(kSettingsTemporary, kSettings) == 0;
  if (!ok) unlink(kSettingsTemporary);
  return ok;
}

/**
 * @brief 通过旧 sysfs GPIO ABI 导出输出脚并保持 value fd 打开。
 *
 * export 后节点由内核异步创建，因此等待窗口限制为约 500 ms。返回的 fd 由
 * DeviceUi::Close() 统一释放，GPIO 本身保留导出状态供下一次启动复用。
 */
int OpenGpio(int number) {
  const std::string base = std::string(kGpioRoot) + "/gpio" + std::to_string(number);
  if (access(base.c_str(), F_OK) != 0) {
    if (!WriteText(std::string(kGpioRoot) + "/export", std::to_string(number))) return -1;
    for (int retry = 0; retry < 50 && access(base.c_str(), F_OK) != 0; ++retry) usleep(10000);
  }
  if (!WriteText(base + "/direction", "out")) return -1;
  return open((base + "/value").c_str(), O_WRONLY | O_CLOEXEC);
}

bool SetGpio(int fd, bool high) {
  const char value = high ? '1' : '0';
  return fd >= 0 && lseek(fd, 0, SEEK_SET) == 0 && WriteAll(fd, &value, 1U);
}

/**
 * @brief 回收摄像头或配网子进程，并在退出迟滞时逐级终止整个进程组。
 *
 * 摄像头命令包含 shell 管道，只等待 shell PID 会留下 v4l2-ctl 或 ffmpeg；负 PID
 * 信号把同组进程一起纳入资源回收。SIGTERM 留出 1 秒正常退出窗口，随后才使用
 * SIGKILL，保证 Close() 不会无限等待外部工具。
 */
int ReapProcessGroup(pid_t child) {
  if (child <= 0) return -1;
  int status = 0;
  pid_t result = waitpid(child, &status, WNOHANG);
  if (result == child) return status;
  if (result < 0 && errno != EINTR) return -1;
  static_cast<void>(kill(-child, SIGTERM));
  for (int retry = 0; retry < 100; ++retry) {
    result = waitpid(child, &status, WNOHANG);
    if (result == child) return status;
    if (result < 0 && errno != EINTR) return -1;
    usleep(10000);
  }
  static_cast<void>(kill(-child, SIGKILL));
  do {
    result = waitpid(child, &status, 0);
  } while (result < 0 && errno == EINTR);
  return result == child ? status : -1;
}

void LogCameraExit(int status) {
  if (status < 0) {
    std::fprintf(stderr, "boompi-ui: camera process status unavailable\n");
  } else if (WIFEXITED(status)) {
    std::fprintf(stderr, "boompi-ui: camera process exit=%d\n",
                 WEXITSTATUS(status));
  } else if (WIFSIGNALED(status)) {
    std::fprintf(stderr, "boompi-ui: camera process signal=%d\n",
                 WTERMSIG(status));
  }
}

unsigned CameraFpsTenths(std::uint64_t frames,
                         std::chrono::milliseconds elapsed) {
  if (elapsed.count() <= 0) return 0U;
  return static_cast<unsigned>(
      frames * 10000U / static_cast<std::uint64_t>(elapsed.count()));
}

void LogCameraStats(std::uint64_t pipeline_frames,
                    std::uint64_t displayed_frames,
                    std::uint64_t dropped_frames,
                    std::chrono::milliseconds frame_span) {
  // FPS 使用首帧到末帧的相邻帧区间，不能混入管线启动或退出清理时间。
  const unsigned pipeline_fps = CameraFpsTenths(
      pipeline_frames > 0U ? pipeline_frames - 1U : 0U, frame_span);
  const unsigned display_fps = CameraFpsTenths(
      displayed_frames > 0U ? displayed_frames - 1U : 0U, frame_span);
  float load_one = 0.0F;
  std::FILE* load = std::fopen("/proc/loadavg", "r");
  const bool have_load = load != nullptr && std::fscanf(load, "%f", &load_one) == 1;
  if (load != nullptr) std::fclose(load);
  if (have_load) {
    std::fprintf(stderr,
                 "boompi-ui: camera target_fps=%u pipeline_fps=%u.%u "
                 "display_fps=%u.%u dropped=%llu load1=%.2f\n",
                 kCameraFps, pipeline_fps / 10U, pipeline_fps % 10U,
                 display_fps / 10U, display_fps % 10U,
                 static_cast<unsigned long long>(dropped_frames), load_one);
  } else {
    std::fprintf(stderr,
                 "boompi-ui: camera target_fps=%u pipeline_fps=%u.%u "
                 "display_fps=%u.%u dropped=%llu load1=unavailable\n",
                 kCameraFps, pipeline_fps / 10U, pipeline_fps % 10U,
                 display_fps / 10U, display_fps % 10U,
                 static_cast<unsigned long long>(dropped_frames));
  }
}

}  // namespace

struct DeviceUi::Impl final {
  // application 写入 View 快照，UI worker 一次取走完整副本，避免状态和字幕跨帧撕裂。
  struct View {
    DeviceUiState state{DeviceUiState::kIdle};
    std::uint8_t volume{60U};
    std::array<std::string, 2> text;
  };
  using CameraFrame = std::array<std::uint16_t, kCameraPixels>;

  // 下列设备 fd 只在 Open()/UI worker/Close() 的受控生命周期内使用。
  int spi{-1}, touch{-1}, data_command{-1}, panel_reset{-1}, backlight{-1};
  // GT911 坐标由 UI worker 更新，LVGL input callback 在同一线程读取。
  int pointer_x{0}, pointer_y{0};
  bool pointer_pressed{false};
  unsigned touch_failures{0U}, touch_recovery_attempts{0U};
  bool touch_disabled{false};
  std::chrono::steady_clock::time_point next_touch_recovery{};
  // stop 是主线程发给 UI worker 的终止信号；action/volume_change 是回程单槽邮箱。
  std::atomic<bool> stop{false};
  std::atomic<int> action{-1}, volume_change{-1};
  // camera_stop 和 camera_pid 允许 UI worker 在管道阻塞期间从外部唤醒 camera worker。
  std::atomic<bool> camera_stop{false};
  std::atomic<pid_t> camera_pid{-1};
  std::atomic<std::uint64_t> camera_displayed_frames{0U};
  pid_t provision_pid{-1};
  std::atomic<LvglScreen::CameraStatus> camera_status{LvglScreen::CameraStatus::kStopped};
  std::mutex view_mutex;
  std::condition_variable wake;
  bool start_done{false}, start_ok{false}, view_dirty{true};
  View view;
  // 摄像头只保留最新完整帧；生产者覆盖旧帧可以限制延迟和内存，且不影响音频时序。
  std::mutex camera_mutex;
  bool camera_frame_ready{false};
  CameraFrame camera_frame{};
  std::thread ui_thread, camera_thread;
  LvglScreen screen;
  lv_disp_draw_buf_t draw_buffer{};
  lv_disp_drv_t display_driver{};
  lv_indev_drv_t input_driver{};
  lv_disp_t* display{nullptr};
  lv_indev_t* input{nullptr};
  std::array<lv_color_t, kScreenWidth * kDrawRows> draw_pixels{};
  std::array<std::uint8_t, 4096> flush_pixels{};

  /** @brief UI worker 原子取得最近一次完整 View，并清除 dirty 标志。 */
  bool TakeView(View* next) {
    std::lock_guard<std::mutex> lock(view_mutex);
    if (!view_dirty) return false;
    *next = view;
    view_dirty = false;
    return true;
  }

  /** @brief 跨线程发布一个 View 成员，并唤醒可能正在等待的 UI worker。 */
  template <typename T>
  void UpdateView(T View::*member, const T& value) {
    std::lock_guard<std::mutex> lock(view_mutex);
    view.*member = value;
    view_dirty = true;
    wake.notify_one();
  }

  /**
   * @brief 从单槽原子邮箱消费最新事件。
   *
   * UI 动作和音量只需要“最近值”语义；exchange 同时完成读取与确认，调用方不会
   * 重复处理同一次触摸。连续事件到达时较新的值覆盖尚未轮询的旧值。
   */
  template <typename T>
  static bool Poll(std::atomic<int>& source, T* result) {
    if (result == nullptr) return false;
    const int value = source.exchange(-1);
    if (value < 0) return false;
    *result = static_cast<T>(value);
    return true;
  }

  /**
   * @brief 按 ST7789P3 的 D/C 时序发送一条命令及可选 payload。
   *
   * D/C 拉低覆盖命令字节，payload 发送前再拉高；整个调用只在 UI worker 中发生，
   * 因而 GPIO 电平和 SPI 字节流不会被其他页面更新交叉打断。
   */
  bool Command(std::uint8_t command, const std::uint8_t* data = nullptr, std::size_t bytes = 0U) {
    if (!SetGpio(data_command, false) || !WriteAll(spi, &command, 1U)) return false;
    return bytes == 0U || (SetGpio(data_command, true) && WriteAll(spi, data, bytes));
  }

  /**
   * @brief 将 LVGL 横屏脏矩形旋转并刷入 ST7789P3 原生坐标。
   *
   * LVGL 的 RGB565 主机字节序像素在这里转成面板要求的高字节在前，并使用固定
   * 4 KiB 暂存块限制栈和单次 write 大小。刷屏失败时熄灭背光并请求 UI worker
   * 退出，防止用户继续看到冻结画面；无论成功与否都必须调用 flush_ready，解除
   * LVGL 内部的绘制等待。
   */
  static void Flush(lv_disp_drv_t* driver, const lv_area_t* area, lv_color_t* pixels) {
    auto* self = static_cast<Impl*>(driver->user_data);
    const int width = area->x2 - area->x1 + 1;
    const int x1 = area->y1, x2 = area->y2;
    const int y1 = kPanelHeight - 1 - area->x2, y2 = kPanelHeight - 1 - area->x1;
    const std::uint8_t columns[] = {static_cast<std::uint8_t>(x1 >> 8), static_cast<std::uint8_t>(x1),
                                    static_cast<std::uint8_t>(x2 >> 8), static_cast<std::uint8_t>(x2)};
    const std::uint8_t rows[] = {static_cast<std::uint8_t>(y1 >> 8), static_cast<std::uint8_t>(y1),
                                 static_cast<std::uint8_t>(y2 >> 8), static_cast<std::uint8_t>(y2)};
    bool ok = self->Command(0x2A, columns, sizeof(columns)) && self->Command(0x2B, rows, sizeof(rows)) &&
              self->Command(0x2C) && SetGpio(self->data_command, true);
    std::size_t used = 0U;
    for (int x = area->x2; ok && x >= area->x1; --x) {
      for (int y = area->y1; y <= area->y2; ++y) {
        const auto pixel = pixels[(y - area->y1) * width + x - area->x1].full;
        self->flush_pixels[used++] = static_cast<std::uint8_t>(pixel >> 8U);
        self->flush_pixels[used++] = static_cast<std::uint8_t>(pixel);
        if (used == self->flush_pixels.size()) {
          ok = WriteAll(self->spi, self->flush_pixels.data(), used);
          used = 0U;
          if (!ok) break;
        }
      }
    }
    if (ok && used != 0U) ok = WriteAll(self->spi, self->flush_pixels.data(), used);
    if (!ok) {
      SetGpio(self->backlight, false);
      self->stop.store(true);
      self->wake.notify_one();
    }
    lv_disp_flush_ready(driver);
  }

  /**
   * @brief 按上电时序复位并初始化 ST7789P3 面板。
   *
   * 背光先保持关闭，避免复位和寄存器配置期间显示随机显存；RESET 的两个 100 ms
   * 窗口以及 Sleep Out 后的 120 ms 来自控制器上电要求。只有 Display On 成功后才
   * 点亮背光，因此 Open() 的成功意味着用户能够看到完整初始化后的页面。
   */
  bool InitPanel() {
    if ((data_command = OpenGpio(kDataCommandGpio)) < 0 || (panel_reset = OpenGpio(kPanelResetGpio)) < 0 ||
        (backlight = OpenGpio(kBacklightGpio)) < 0) return false;
    SetGpio(backlight, false);
    SetGpio(panel_reset, false);
    usleep(100000);
    if (!SetGpio(panel_reset, true)) return false;
    usleep(100000);
    // 初始化表绑定当前面板模组；每组数据依次为 command、payload size、payload。
    // 维护时应对照模组厂商序列整体更新，单独猜测 gamma 或电源寄存器会产生隐蔽色偏。
    static constexpr std::uint8_t init[] = {
        0xB2, 5, 0x0C, 0x0C, 0, 0x33, 0x33, 0x36, 1, 0, 0x3A, 1, 0x55, 0xB7, 1, 0x55, 0xBB, 1, 0x1A,
        0xC0, 1, 0x2C, 0xC2, 1, 1, 0xC3, 1, 0x19, 0xC6, 1, 0x0F, 0xD0, 1, 0xA7, 0xD0, 2, 0xA4, 0xA1,
        0xD6, 1, 0xA1, 0xE0, 14, 0xF0, 3, 9, 0x0B, 0x0A, 0x16, 0x2B, 0x33, 0x41, 0x38, 0x14, 0x14,
        0x29, 0x2F, 0xE1, 14, 0xF0, 4, 6, 9, 8, 4, 0x2B, 0x32, 0x41, 0x36, 0x12, 0x12, 0x2A, 0x30,
        0x21, 0, 0x2A, 4, 0, 0, 0, 0xEF, 0x2B, 4, 0, 0, 1, 0x3F};
    for (std::size_t at = 0U; at < sizeof(init);) {
      const std::uint8_t size = init[at + 1U];
      if (!Command(init[at], init + at + 2U, size)) return false;
      at += size + 2U;
    }
    if (!Command(0x11)) return false;
    usleep(120000);
    return Command(0x29) && SetGpio(backlight, true);
  }

  /**
   * @brief 复位 GT911、锁定 0x14 地址并打开 I2C-3。
   *
   * GT911 在 RESET 上升沿采样 INT 电平；这里用 INT=1 选择 0x14，随后把该 GPIO
   * 恢复为输入。初始化结束前先试读状态寄存器，避免把线路故障延迟到 LVGL 循环。
   */
  bool InitTouch() {
    const int touch_reset = OpenGpio(kTouchResetGpio), interrupt = OpenGpio(kTouchInterruptGpio);
    if (touch_reset < 0 || interrupt < 0) {
      if (touch_reset >= 0) close(touch_reset);
      if (interrupt >= 0) close(interrupt);
      return false;
    }
    bool reset_ok = SetGpio(touch_reset, false);
    usleep(20000);
    reset_ok = SetGpio(interrupt, true) && reset_ok;  // RESET 上升沿前用 INT=1 选择地址 0x14。
    usleep(2000);
    reset_ok = SetGpio(touch_reset, true) && reset_ok;
    usleep(6000);
    reset_ok = SetGpio(interrupt, false) && reset_ok;
    usleep(50000);
    close(touch_reset);
    close(interrupt);
    if (!reset_ok) return false;
    const std::string irq = std::string(kGpioRoot) + "/gpio" + std::to_string(kTouchInterruptGpio) + "/direction";
    if (!WriteText(irq, "in")) return false;
    usleep(60000);
    touch = open("/dev/i2c-3", O_RDWR | O_CLOEXEC);
    std::uint8_t status = 0U;
    if (touch >= 0 && ReadTouch(kTouchStatus, &status, 1U)) return true;
    if (touch >= 0) close(touch);
    touch = -1;
    return false;
  }

  /**
   * @brief 使用一次 I2C_RDWR combined transfer 读取 GT911 寄存器。
   *
   * 寄存器地址为大端 16 位，write/read 两条 message 之间保持 repeated-start，避免
   * STOP 让控制器丢失当前地址。调用方与恢复逻辑都属于 UI worker，无需额外锁。
   */
  bool ReadTouch(std::uint16_t address, std::uint8_t* data, std::size_t size) {
    std::uint8_t reg[] = {static_cast<std::uint8_t>(address >> 8U), static_cast<std::uint8_t>(address)};
    i2c_msg messages[] = {{kGt911Address, 0, 2, reg},
                          {kGt911Address, I2C_M_RD, static_cast<__u16>(size), data}};
    i2c_rdwr_ioctl_data transfer{messages, 2U};
    return ioctl(touch, I2C_RDWR, &transfer) == 2;
  }

  /** @brief 写零确认本轮触摸数据，使 GT911 可以发布下一组坐标。 */
  bool ClearTouchStatus() {
    std::uint8_t clear[] = {0x81, 0x4E, 0};
    i2c_msg message{kGt911Address, 0, 3, clear};
    i2c_rdwr_ioctl_data transfer{&message, 1U};
    return ioctl(touch, I2C_RDWR, &transfer) == 1;
  }

  /**
   * @brief 对连续 I2C 故障执行有界复位，保护 UI 循环免受永久重试占用。
   *
   * 单次瞬态错误只释放按压状态；连续三次后最多复位两轮，每轮间隔两秒。耗尽预算
   * 后保持显示可用并禁用触摸，错误会在日志中明确暴露，音频和页面刷新可以继续。
   */
  void TouchFailed(const char* stage) {
    pointer_pressed = false;
    if (touch_disabled) return;
    ++touch_failures;
    if (touch_failures < kTouchFailureLimit) return;

    const auto now = std::chrono::steady_clock::now();
    if (now < next_touch_recovery) return;
    if (touch_recovery_attempts >= kTouchRecoveryLimit) {
      touch_disabled = true;
      std::fprintf(stderr, "boompi-ui: GT911 disabled after bounded recovery\n");
      return;
    }

    ++touch_recovery_attempts;
    next_touch_recovery = now + std::chrono::seconds(2);
    std::fprintf(stderr, "boompi-ui: GT911 %s failed; recovery %u/%u\n",
                 stage, touch_recovery_attempts, kTouchRecoveryLimit);
    if (touch >= 0) close(touch);
    touch = -1;
    if (InitTouch()) {
      touch_failures = 0U;
      touch_recovery_attempts = 0U;
      std::fprintf(stderr, "boompi-ui: GT911 recovered\n");
    } else if (touch_recovery_attempts == kTouchRecoveryLimit) {
      touch_disabled = true;
      std::fprintf(stderr, "boompi-ui: GT911 disabled after bounded recovery\n");
    }
  }

  /**
   * @brief 读取一组 GT911 点数据并转换到 320x240 横屏坐标。
   *
   * 当前产品只消费第一触点。原生面板的 x/y 轴与横屏 LVGL 坐标交换，其中横向还
   * 需要反转；每组 ready 数据在处理后显式清状态，遗漏确认会让控制器停留在旧点。
   */
  void PollTouch() {
    if (touch_disabled) {
      pointer_pressed = false;
      return;
    }
    std::uint8_t status = 0U;
    if (!ReadTouch(kTouchStatus, &status, 1U)) {
      TouchFailed("status read");
      return;
    }
    if ((status & 0x80U) == 0U) {
      touch_failures = 0U;
      touch_recovery_attempts = 0U;
      return;
    }
    const unsigned count = status & 0x0FU;
    if (count == 0U) pointer_pressed = false;
    else if (count <= 5U) {
      std::array<std::uint8_t, 8> point{};
      if (!ReadTouch(kTouchPoint, point.data(), point.size())) {
        TouchFailed("point read");
        return;
      }
      const int x = point[1] | (static_cast<int>(point[2]) << 8);
      const int y = point[3] | (static_cast<int>(point[4]) << 8);
      pointer_y = std::clamp(x, 0, kPanelWidth - 1);
      pointer_x = kPanelHeight - 1 - std::clamp(y, 0, kPanelHeight - 1);
      pointer_pressed = true;
    } else {
      pointer_pressed = false;
    }
    if (!ClearTouchStatus()) {
      TouchFailed("status clear");
      return;
    }
    touch_failures = 0U;
    touch_recovery_attempts = 0U;
  }

  /**
   * @brief LVGL pointer 回调，在 UI worker 内同步采样 GT911。
   *
   * LVGL 会从 lv_timer_handler() 调用这里，坐标、按压状态和页面事件因此保持同一
   * 线程顺序，不需要把 lv_indev_data_t 暴露给其他线程。
   */
  static void ReadInput(lv_indev_drv_t* driver, lv_indev_data_t* data) {
    auto* self = static_cast<Impl*>(driver->user_data);
    self->PollTouch();
    data->point.x = static_cast<lv_coord_t>(self->pointer_x);
    data->point.y = static_cast<lv_coord_t>(self->pointer_y);
    data->state = self->pointer_pressed ? LV_INDEV_STATE_PR : LV_INDEV_STATE_REL;
  }

  /**
   * @brief 从二维码页面启动唯一一个 Wi-Fi 配网进程。
   *
   * 二维码只携带临时热点凭据；boompi-provision 负责 AP、DHCP/DNS 和凭据保存。
   * --no-panel 保证子进程不会与本进程争用 SPI/LVGL。setsid() 建立独立进程组，
   * UI 退出时能够统一终止脚本派生的服务。重复点击在现有 child 退出前会被忽略。
   */
  void StartProvisioning() {
    if (provision_pid > 0) return;
    if (access("/sys/class/net/wlan0", F_OK) != 0 || access(kProvision, X_OK) != 0) {
      screen.SetProvisionMessage("配网服务不可用", true);
      return;
    }
    const pid_t child = fork();
    if (child == 0) {
      if (setsid() < 0) _exit(126);
      execl(kProvision, "boompi-provision", "--no-panel", static_cast<char*>(nullptr));
      _exit(127);
    }
    if (child > 0)
      provision_pid = child;
    else
      screen.SetProvisionMessage("配网服务启动失败", true);
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
    if (event == LvglScreen::Event::kWake)
      self->action.store(static_cast<int>(DeviceUiAction::kWake));
    else if (event == LvglScreen::Event::kInterrupt)
      self->action.store(static_cast<int>(DeviceUiAction::kInterrupt));
    else if (event == LvglScreen::Event::kProvision)
      self->StartProvisioning();
    else if (event == LvglScreen::Event::kCameraOn) self->StartCamera();
    else if (event == LvglScreen::Event::kCameraOff) self->StopCamera();
    else {
      self->volume_change.store(value);
      if (event == LvglScreen::Event::kVolumeCommit && !SaveVolume(value))
        std::fprintf(stderr, "boompi-ui: volume setting save failed\n");
    }
  }

  void CameraError(const char* reason) {
    std::fprintf(stderr, "boompi-ui: camera %s\n", reason);
    {
      std::lock_guard<std::mutex> lock(camera_mutex);
      camera_frame_ready = false;
      camera_frame.fill(0U);
    }
    camera_status.store(LvglScreen::CameraStatus::kError);
    wake.notify_one();
  }

  /**
   * @brief 让 UI worker 取得 camera worker 发布的最近一帧。
   *
   * 单槽交接主动丢弃来不及显示的旧帧，使预览延迟受控；仅完整的 320x180 帧会把
   * ready 置位，因此 LVGL 永远不会接触正在写入或长度不足的图像。
   */
  bool TakeCameraFrame(CameraFrame* frame) {
    std::lock_guard<std::mutex> lock(camera_mutex);
    if (!camera_frame_ready) return false;
    *frame = camera_frame;
    camera_frame_ready = false;
    return true;
  }

  /**
   * @brief 在 camera worker 中运行 SC3336 转换管线并组装定长 RGB565 帧。
   *
   * pipe stdout 是唯一帧数据通道，stderr 分别落到诊断日志。read() 可能拆分一帧，
   * used 累积到精确的 kCameraBytes 后才在短锁内发布。首帧允许两秒启动，稳定后每
   * 帧最多等待一秒；EOF、短流或超时都会切换为错误状态并回收整个子进程组。
   */
  void CaptureCamera() {
    int output[2]{};
    if (pipe(output) != 0) {
      CameraError("pipe failed");
      return;
    }
    const pid_t child = fork();
    if (child == 0) {
      close(output[0]);
      setpgid(0, 0);
      if (dup2(output[1], STDOUT_FILENO) < 0) _exit(126);
      close(output[1]);
      execl("/bin/sh", "sh", "-c", kCameraCommand, static_cast<char*>(nullptr));
      _exit(127);
    }
    close(output[1]);
    if (child < 0) {
      close(output[0]);
      CameraError("fork failed");
      return;
    }
    static_cast<void>(setpgid(child, child));
    camera_pid.store(child);
    CameraFrame captured{};
    auto* bytes = reinterpret_cast<std::uint8_t*>(captured.data());
    std::size_t used = 0U;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    const auto session_started = std::chrono::steady_clock::now();
    auto report_at = session_started + kCameraReportPeriod;
    auto first_frame_at = std::chrono::steady_clock::time_point{};
    auto last_frame_at = std::chrono::steady_clock::time_point{};
    std::uint64_t pipeline_frames = 0U;
    std::uint64_t dropped_frames = 0U;
    const char* failure = nullptr;
    bool have_frame = false;
    while (!camera_stop.load()) {
      const auto now = std::chrono::steady_clock::now();
      if (now >= deadline) {
        failure = have_frame ? "frame timeout" : "first frame timeout";
        break;
      }
      int timeout_ms = static_cast<int>(
          std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count());
      if (timeout_ms < 1) timeout_ms = 1;
      pollfd descriptor{output[0], POLLIN, 0};
      int ready = 0;
      do {
        ready = ::poll(&descriptor, 1U, timeout_ms);
      } while (ready < 0 && errno == EINTR && !camera_stop.load());
      if (camera_stop.load()) break;
      if (ready == 0) {
        failure = have_frame ? "frame timeout" : "first frame timeout";
        break;
      }
      if (ready < 0 || (descriptor.revents & POLLIN) == 0) {
        failure = "stream ended";
        break;
      }
      const ssize_t count = read(output[0], bytes + used, kCameraBytes - used);
      if (count < 0 && errno == EINTR) continue;
      if (count <= 0) {
        failure = "stream read failed";
        break;
      }
      used += static_cast<std::size_t>(count);
      if (used == kCameraBytes) {
        const auto frame_ready_at = std::chrono::steady_clock::now();
        ++pipeline_frames;
        if (pipeline_frames == 1U) first_frame_at = frame_ready_at;
        last_frame_at = frame_ready_at;
        {
          std::lock_guard<std::mutex> lock(camera_mutex);
          if (camera_frame_ready) ++dropped_frames;
          camera_frame = captured;
          camera_frame_ready = true;
        }
        camera_status.store(LvglScreen::CameraStatus::kLive);
        wake.notify_one();
        used = 0U;
        have_frame = true;
        deadline = frame_ready_at + std::chrono::seconds(1);
        if (frame_ready_at >= report_at) {
          LogCameraStats(
              pipeline_frames, camera_displayed_frames.load(), dropped_frames,
              std::chrono::duration_cast<std::chrono::milliseconds>(
                  last_frame_at - first_frame_at));
          report_at = frame_ready_at + kCameraReportPeriod;
        }
      }
    }
    close(output[0]);
    if (!camera_stop.load() && failure != nullptr) CameraError(failure);
    LogCameraExit(ReapProcessGroup(child));
    if (pipeline_frames != 0U) {
      LogCameraStats(
          pipeline_frames, camera_displayed_frames.load(), dropped_frames,
          std::chrono::duration_cast<std::chrono::milliseconds>(
              last_frame_at - first_frame_at));
    }
    camera_pid.store(-1);
  }

  /** @brief 清理上一轮 worker 后启动一次新的 SC3336 预览会话。 */
  void StartCamera() {
    if (camera_thread.joinable()) camera_thread.join();
    {
      std::lock_guard<std::mutex> lock(camera_mutex);
      camera_frame_ready = false;
      camera_frame.fill(0U);
    }
    camera_displayed_frames.store(0U);
    camera_stop.store(false);
    camera_status.store(LvglScreen::CameraStatus::kStarting);
    try {
      camera_thread = std::thread([this] { CaptureCamera(); });
    } catch (...) {
      CameraError("worker could not start");
    }
  }

  /**
   * @brief 终止采集管线、等待 worker 退出并清空待显示帧。
   *
   * 先设置 stop，再向进程组发送信号，可以同时覆盖正常轮询退出和阻塞在外部工具
   * 的情况。join 完成后才清帧和发布 Stopped，保证返回时 /dev/video14 已经释放。
   */
  void StopCamera() {
    camera_stop.store(true);
    const pid_t child = camera_pid.load();
    if (child > 0) {
      static_cast<void>(kill(-child, SIGTERM));
      usleep(100000);
      if (camera_pid.load() == child) static_cast<void>(kill(-child, SIGKILL));
    }
    if (camera_thread.joinable()) camera_thread.join();
    std::lock_guard<std::mutex> lock(camera_mutex);
    camera_frame_ready = false;
    camera_frame.fill(0U);
    camera_status.store(LvglScreen::CameraStatus::kStopped);
  }

  /**
   * @brief UI worker 主循环，唯一拥有全部 LVGL 对象和页面状态。
   *
   * nice(5) 让显示与摄像头工作在普通低优先级，避免和 SCHED_FIFO 音频线程竞争。
   * 循环依次回收配网进程、推进 LVGL tick、应用 application 快照、接收摄像头帧并
   * 执行 timer handler。condition_variable 最多等待 30 ms，使空闲时不忙轮询，
   * 新状态、摄像头帧或关闭请求又能立即唤醒。所有 LVGL API 都留在这个循环及其
   * 同步事件回调中，camera/application 线程从不直接访问 lv_obj_t。
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
    const bool ready = display != nullptr && input != nullptr && access(font, R_OK) == 0 && screen.Create(font);
    if (ready) screen.SetEventHandler(HandleEvent, this);
    {
      std::lock_guard<std::mutex> lock(view_mutex);
      start_ok = ready;
      start_done = true;
    }
    wake.notify_one();
    if (!ready) {
      screen.Destroy();
      if (input != nullptr) lv_indev_delete(input);
      if (display != nullptr) lv_disp_remove(display);
      return;
    }
    auto shown_camera = LvglScreen::CameraStatus::kStopped;
    auto tick = std::chrono::steady_clock::now();
    auto last_frame = tick;
    bool have_display_frame = false;
    CameraFrame display_frame{};
    while (!stop.load()) {
      if (provision_pid > 0) {
        int status = 0;
        if (waitpid(provision_pid, &status, WNOHANG) == provision_pid) {
          const bool saved = WIFEXITED(status) && WEXITSTATUS(status) == 0;
          screen.SetProvisionMessage(saved ? "Wi-Fi 已保存，正在连接"
                                           : "配网服务启动失败",
                                     !saved);
          provision_pid = -1;
        }
      }
      const auto now = std::chrono::steady_clock::now();
      const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - tick).count();
      tick = now;
      if (elapsed > 0) lv_tick_inc(static_cast<std::uint32_t>(elapsed));
      View next;
      if (TakeView(&next)) {
        screen.SetState(next.state);
        screen.SetVolume(next.volume);
        screen.SetText(next.text[0], next.text[1]);
      }
      const auto status = camera_status.load();
      if (status != shown_camera) {
        shown_camera = status;
        if (status == LvglScreen::CameraStatus::kStarting) {
          last_frame = now;
          have_display_frame = false;
        }
        screen.SetCameraStatus(status);
      }
      const bool new_frame = TakeCameraFrame(&display_frame);
      if (new_frame && status == LvglScreen::CameraStatus::kLive) {
        // FPS 基于真正交给页面的相邻帧计算，表示用户看到的预览吞吐量。
        const auto frame_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_frame).count();
        const unsigned fps_tenths = have_display_frame && frame_ms > 0
                                         ? static_cast<unsigned>(10000 / frame_ms)
                                         : kCameraFps * 10U;
        last_frame = now;
        have_display_frame = true;
        screen.SetCameraFrame(display_frame.data(), display_frame.size(), fps_tenths);
        camera_displayed_frames.fetch_add(1U);
        // 摄像头帧到达时立即刷新，避免等待普通 30 ms UI 周期额外增加预览延迟。
        lv_refr_now(display);
      }
      static_cast<void>(lv_timer_handler());
      std::unique_lock<std::mutex> lock(view_mutex);
      wake.wait_for(lock, std::chrono::milliseconds(30));
    }
    if (provision_pid > 0) {
      static_cast<void>(ReapProcessGroup(provision_pid));
      provision_pid = -1;
    }
    StopCamera();
    // LVGL 对象先于已注册的 input/display 删除，销毁期间仍可访问活动 display。
    screen.Destroy();
    if (input != nullptr) lv_indev_delete(input);
    if (display != nullptr) lv_disp_remove(display);
  }
};

DeviceUi::~DeviceUi() noexcept { Close(); }

std::uint8_t DeviceUi::LoadVolume(std::uint8_t fallback) noexcept {
  unsigned saved = 0U;
  std::FILE* file = std::fopen(kSettings, "r");
  if (file == nullptr) return std::min<std::uint8_t>(fallback, 100U);
  const bool valid = std::fscanf(file, "%u", &saved) == 1 && saved <= 100U;
  std::fclose(file);
  return valid ? static_cast<std::uint8_t>(saved) : std::min<std::uint8_t>(fallback, 100U);
}

bool DeviceUi::Open() noexcept {
  Close();
  impl_ = new (std::nothrow) Impl;
  if (impl_ == nullptr) return false;
  impl_->spi = open("/dev/spidev0.0", O_RDWR | O_CLOEXEC);
  std::uint8_t mode = 0U;
  std::uint8_t bits = 8U;
  std::uint32_t speed = kSpiSpeedHz;
  // 外设按 SPI参数 → 面板 → 触摸的顺序建立；任一步失败都走 Close() 的统一回收。
  const bool ready = impl_->spi >= 0 && ioctl(impl_->spi, SPI_IOC_WR_MODE, &mode) == 0 &&
      ioctl(impl_->spi, SPI_IOC_WR_BITS_PER_WORD, &bits) == 0 &&
      ioctl(impl_->spi, SPI_IOC_WR_MAX_SPEED_HZ, &speed) == 0 &&
      ioctl(impl_->spi, SPI_IOC_RD_MAX_SPEED_HZ, &speed) == 0 &&
      impl_->InitPanel() && impl_->InitTouch();
  if (!ready) {
    Close();
    return false;
  }
  std::fprintf(stderr, "boompi-ui: SPI=%u Hz; touch=GT911/i2c-3\n", speed);
  try {
    impl_->ui_thread = std::thread([state = impl_] { state->Run(); });
  } catch (...) {
    Close();
    return false;
  }
  // Open() 等待 UI worker 完成字体和页面创建，调用方不会向半初始化对象发布状态。
  std::unique_lock<std::mutex> lock(impl_->view_mutex);
  const bool started = impl_->wake.wait_for(lock, std::chrono::seconds(2),
                                            [this] { return impl_->start_done; }) && impl_->start_ok;
  lock.unlock();
  if (!started) Close();
  return started;
}

void DeviceUi::SetState(DeviceUiState state) noexcept {
  if (impl_ != nullptr) impl_->UpdateView(&Impl::View::state, state);
}

void DeviceUi::SetText(std::string_view first, std::string_view second) noexcept {
  if (impl_ == nullptr) return;
  impl_->UpdateView(&Impl::View::text, std::array<std::string, 2>{std::string(first), std::string(second)});
}

bool DeviceUi::PollAction(DeviceUiAction* result) noexcept {
  return impl_ != nullptr && Impl::Poll(impl_->action, result);
}

bool DeviceUi::PollVolumeChange(std::uint8_t* result) noexcept {
  return impl_ != nullptr && Impl::Poll(impl_->volume_change, result);
}

void DeviceUi::SetVolume(std::uint8_t percent) noexcept {
  if (impl_ != nullptr) impl_->UpdateView(&Impl::View::volume, std::min<std::uint8_t>(percent, 100U));
}

void DeviceUi::Close() noexcept {
  if (impl_ == nullptr) return;
  // UI worker 负责停止 camera/provision 并销毁 LVGL；join 后关闭硬件 fd 可避免回调悬空。
  impl_->stop.store(true);
  impl_->wake.notify_one();
  if (impl_->ui_thread.joinable()) impl_->ui_thread.join();
  SetGpio(impl_->backlight, false);
  const int descriptors[] = {impl_->touch, impl_->spi, impl_->data_command, impl_->panel_reset, impl_->backlight};
  for (int fd : descriptors) if (fd >= 0) close(fd);
  delete impl_;
  impl_ = nullptr;
}

}  // namespace boompi::ui
