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

constexpr int kPanelWidth = 240, kPanelHeight = 320;
constexpr int kScreenWidth = 320, kScreenHeight = 240, kDrawRows = 32;
constexpr int kBacklightGpio = 53, kDataCommandGpio = 66, kPanelResetGpio = 67;
constexpr int kTouchResetGpio = 64, kTouchInterruptGpio = 73;
constexpr int kGt911Address = 0x14;
constexpr std::uint16_t kTouchStatus = 0x814E;
constexpr std::uint16_t kTouchPoint = 0x814F;
constexpr std::size_t kCameraPixels = 320U * 180U;
constexpr std::size_t kCameraBytes = kCameraPixels * sizeof(std::uint16_t);
constexpr unsigned kTouchFailureLimit = 3U;
constexpr unsigned kTouchRecoveryLimit = 2U;
constexpr unsigned kSpiSpeedHz = 80000000U;
constexpr char kGpioRoot[] = "/sys/class/gpio";
constexpr char kSettings[] = "/userdata/boompi/config/ui.settings";
constexpr char kSettingsTemporary[] = "/userdata/boompi/config/ui.settings.tmp";
constexpr char kProvision[] = "/usr/sbin/boompi-provision";
constexpr char kFont[] = "/userdata/boompi/fonts/NotoSansCJK-Regular.ttc";
constexpr char kFallbackFont[] = "/oem/usr/share/simsun_en.ttf";
constexpr char kCameraCommand[] =
    "/usr/bin/v4l2-ctl -d /dev/video14 "
    "--set-fmt-video=width=576,height=324,pixelformat=NV12 "
    "--stream-mmap=4 --stream-to=- 2>/run/boompi-camera-v4l2.log | "
    "/usr/bin/ffmpeg -hide_banner -loglevel error -f rawvideo -pixel_format nv12 "
    "-video_size 576x324 -framerate 25 -i pipe:0 -vf 'scale=320:180:flags=fast_bilinear' "
    "-pix_fmt rgb565le -f rawvideo pipe:1 2>/run/boompi-camera-ffmpeg.log";

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

}  // namespace

struct DeviceUi::Impl final {
  struct View {
    DeviceUiState state{DeviceUiState::kIdle};
    std::uint8_t volume{60U};
    std::array<std::string, 2> text;
  };
  using CameraFrame = std::array<std::uint16_t, kCameraPixels>;

  int spi{-1}, touch{-1}, data_command{-1}, panel_reset{-1}, backlight{-1};
  int pointer_x{0}, pointer_y{0};
  bool pointer_pressed{false};
  unsigned touch_failures{0U}, touch_recovery_attempts{0U};
  bool touch_disabled{false};
  std::chrono::steady_clock::time_point next_touch_recovery{};
  std::atomic<bool> stop{false};
  std::atomic<int> action{-1}, volume_change{-1};
  std::atomic<bool> camera_stop{false};
  std::atomic<pid_t> camera_pid{-1};
  pid_t provision_pid{-1};
  std::atomic<LvglScreen::CameraStatus> camera_status{LvglScreen::CameraStatus::kStopped};
  std::mutex view_mutex;
  std::condition_variable wake;
  bool start_done{false}, start_ok{false}, view_dirty{true};
  View view;
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

  bool TakeView(View* next) {
    std::lock_guard<std::mutex> lock(view_mutex);
    if (!view_dirty) return false;
    *next = view;
    view_dirty = false;
    return true;
  }

  template <typename T>
  void UpdateView(T View::*member, const T& value) {
    std::lock_guard<std::mutex> lock(view_mutex);
    view.*member = value;
    view_dirty = true;
    wake.notify_one();
  }

  template <typename T>
  static bool Poll(std::atomic<int>& source, T* result) {
    if (result == nullptr) return false;
    const int value = source.exchange(-1);
    if (value < 0) return false;
    *result = static_cast<T>(value);
    return true;
  }

  bool Command(std::uint8_t command, const std::uint8_t* data = nullptr, std::size_t bytes = 0U) {
    if (!SetGpio(data_command, false) || !WriteAll(spi, &command, 1U)) return false;
    return bytes == 0U || (SetGpio(data_command, true) && WriteAll(spi, data, bytes));
  }

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

  bool InitPanel() {
    if ((data_command = OpenGpio(kDataCommandGpio)) < 0 || (panel_reset = OpenGpio(kPanelResetGpio)) < 0 ||
        (backlight = OpenGpio(kBacklightGpio)) < 0) return false;
    SetGpio(backlight, false);
    SetGpio(panel_reset, false);
    usleep(100000);
    if (!SetGpio(panel_reset, true)) return false;
    usleep(100000);
    // 每组数据依次为 command、payload size、payload。
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

  bool InitTouch() {
    const int touch_reset = OpenGpio(kTouchResetGpio), interrupt = OpenGpio(kTouchInterruptGpio);
    if (touch_reset < 0 || interrupt < 0) {
      if (touch_reset >= 0) close(touch_reset);
      if (interrupt >= 0) close(interrupt);
      return false;
    }
    bool reset_ok = SetGpio(touch_reset, false);
    usleep(20000);
    reset_ok = SetGpio(interrupt, true) && reset_ok;  // INT=1 选择地址 0x14。
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

  bool ReadTouch(std::uint16_t address, std::uint8_t* data, std::size_t size) {
    std::uint8_t reg[] = {static_cast<std::uint8_t>(address >> 8U), static_cast<std::uint8_t>(address)};
    i2c_msg messages[] = {{kGt911Address, 0, 2, reg},
                          {kGt911Address, I2C_M_RD, static_cast<__u16>(size), data}};
    i2c_rdwr_ioctl_data transfer{messages, 2U};
    return ioctl(touch, I2C_RDWR, &transfer) == 2;
  }

  bool ClearTouchStatus() {
    std::uint8_t clear[] = {0x81, 0x4E, 0};
    i2c_msg message{kGt911Address, 0, 3, clear};
    i2c_rdwr_ioctl_data transfer{&message, 1U};
    return ioctl(touch, I2C_RDWR, &transfer) == 1;
  }

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

  static void ReadInput(lv_indev_drv_t* driver, lv_indev_data_t* data) {
    auto* self = static_cast<Impl*>(driver->user_data);
    self->PollTouch();
    data->point.x = static_cast<lv_coord_t>(self->pointer_x);
    data->point.y = static_cast<lv_coord_t>(self->pointer_y);
    data->state = self->pointer_pressed ? LV_INDEV_STATE_PR : LV_INDEV_STATE_REL;
  }

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
    camera_status.store(LvglScreen::CameraStatus::kError);
    wake.notify_one();
  }

  bool TakeCameraFrame(CameraFrame* frame) {
    std::lock_guard<std::mutex> lock(camera_mutex);
    if (!camera_frame_ready) return false;
    *frame = camera_frame;
    camera_frame_ready = false;
    return true;
  }

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
        std::lock_guard<std::mutex> lock(camera_mutex);
        camera_frame = captured;
        camera_frame_ready = true;
        used = 0U;
        have_frame = true;
        deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        camera_status.store(LvglScreen::CameraStatus::kLive);
        wake.notify_one();
      }
    }
    close(output[0]);
    if (!camera_stop.load() && failure != nullptr) CameraError(failure);
    LogCameraExit(ReapProcessGroup(child));
    camera_pid.store(-1);
  }

  void StartCamera() {
    if (camera_thread.joinable()) camera_thread.join();
    camera_stop.store(false);
    camera_status.store(LvglScreen::CameraStatus::kStarting);
    try {
      camera_thread = std::thread([this] { CaptureCamera(); });
    } catch (...) {
      CameraError("worker could not start");
    }
  }

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
    camera_status.store(LvglScreen::CameraStatus::kStopped);
  }

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
        if (status == LvglScreen::CameraStatus::kStarting) last_frame = now;
        screen.SetCameraStatus(status);
      }
      const bool new_frame = TakeCameraFrame(&display_frame);
      if (new_frame && status == LvglScreen::CameraStatus::kLive) {
        const auto frame_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_frame).count();
        const unsigned fps_tenths = frame_ms > 0 ? static_cast<unsigned>(10000 / frame_ms) : 0U;
        last_frame = now;
        screen.SetCameraFrame(display_frame.data(), display_frame.size(), fps_tenths);
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
