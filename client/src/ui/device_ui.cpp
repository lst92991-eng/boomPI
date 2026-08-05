#include "boompi/ui/device_ui.h"

#include "boompi/ui/lvgl_screen.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cctype>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <linux/i2c.h>
#include <linux/input.h>
#include <linux/spi/spidev.h>
#include <mutex>
#include <new>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

namespace boompi::ui {
namespace {

constexpr int kPanelWidth = 240;
constexpr int kPanelHeight = 320;
constexpr int kLogicalWidth = 320;
constexpr int kLogicalHeight = 240;
constexpr int kDrawBufferRows = 32;
constexpr int kTouchEventLimit = 32;
constexpr unsigned kSpiSpeedHz = 80000000U;
constexpr int kBacklightGpio = 53;
constexpr int kDcGpio = 66;
constexpr int kResetGpio = 67;
constexpr int kTouchResetGpio = 64;
constexpr int kTouchInterruptGpio = 73;
constexpr int kGt911Address = 0x14;
constexpr std::uint16_t kGt911StatusRegister = 0x814E;
constexpr std::uint16_t kGt911PointRegister = 0x814F;
constexpr std::size_t kActionQueueCapacity = 8U;
constexpr std::size_t kCameraWidth = 320U;
constexpr std::size_t kCameraHeight = 180U;
constexpr std::size_t kCameraPixelCount = kCameraWidth * kCameraHeight;
constexpr std::size_t kCameraFrameBytes =
    kCameraPixelCount * sizeof(std::uint16_t);
constexpr std::chrono::milliseconds kUiStartTimeout{3000};
constexpr std::chrono::milliseconds kUiRefreshPeriod{33};  // 30 FPS on 80 MHz SPI.
constexpr char kGpioRoot[] = "/sys/class/gpio";
constexpr char kGt911I2cDevice[] = "/dev/i2c-3";
constexpr char kPrimaryFont[] = "/userdata/boompi/fonts/NotoSansCJK-Regular.ttc";
constexpr char kFallbackFont[] = "/oem/usr/share/simsun_en.ttf";
constexpr char kIconFont[] = "/userdata/boompi/fonts/tabler-icons-200-subset.ttf";
constexpr char kUiSettings[] = "/userdata/boompi/config/ui.settings";
constexpr char kUiSettingsTemporary[] =
    "/userdata/boompi/config/ui.settings.tmp";
constexpr char kCameraPipeline[] =
    "/usr/bin/v4l2-ctl -d /dev/video14 "
    "--set-fmt-video=width=576,height=324,pixelformat=NV12 "
    "--stream-mmap=4 --stream-to=- 2>/run/boompi-camera-v4l2.log | "
    "/usr/bin/ffmpeg -hide_banner -loglevel error -f rawvideo "
    "-pixel_format nv12 -video_size 576x324 -framerate 25 -i pipe:0 "
    "-vf 'fps=4,scale=320:180' -pix_fmt rgb565le -f rawvideo pipe:1 "
    "2>/run/boompi-camera-ffmpeg.log";

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

bool WriteText(const std::string& path, const std::string& text) {
  const int fd = open(path.c_str(), O_WRONLY | O_CLOEXEC);
  if (fd < 0) return false;
  const bool ok = WriteAll(fd, text.data(), text.size());
  close(fd);
  return ok;
}

int OpenGpio(const std::string& root, int number) {
  const std::string base = root + "/gpio" + std::to_string(number);
  if (access(base.c_str(), F_OK) != 0) {
    if (!WriteText(root + "/export", std::to_string(number))) return -1;
    for (int retry = 0; retry < 50 && access(base.c_str(), F_OK) != 0; ++retry)
      usleep(10000);
  }
  if (!WriteText(base + "/direction", "out")) return -1;
  return open((base + "/value").c_str(), O_WRONLY | O_CLOEXEC);
}

bool SetGpio(int fd, bool high) {
  const char value = high ? '1' : '0';
  return fd >= 0 && lseek(fd, 0, SEEK_SET) == 0 && WriteAll(fd, &value, 1U);
}

// The current board answers at 0x14 while its flashed DT still describes
// 0x5d.  Reproduce Goodix's documented address-selection/reset sequence
// before using the userspace polling fallback.
bool ResetGt911AtAddress14() {
  const int reset = OpenGpio(kGpioRoot, kTouchResetGpio);
  const int interrupt = OpenGpio(kGpioRoot, kTouchInterruptGpio);
  if (reset < 0 || interrupt < 0) {
    if (reset >= 0) close(reset);
    if (interrupt >= 0) close(interrupt);
    return false;
  }
  bool ok = SetGpio(reset, false);
  usleep(20000);
  ok = SetGpio(interrupt, true) && ok;
  usleep(2000);
  ok = SetGpio(reset, true) && ok;
  usleep(6000);
  ok = SetGpio(interrupt, false) && ok;
  usleep(50000);
  close(interrupt);
  close(reset);
  ok = WriteText(std::string(kGpioRoot) + "/gpio" +
                     std::to_string(kTouchInterruptGpio) + "/direction",
                 "in") && ok;
  usleep(60000);
  return ok;
}

bool IsGt911Name(char* name) {
  for (char* cursor = name; *cursor != '\0'; ++cursor) {
    *cursor = static_cast<char>(std::tolower(static_cast<unsigned char>(*cursor)));
  }
  return std::strstr(name, "gt911") != nullptr ||
         std::strstr(name, "goodix") != nullptr;
}

int OpenGt911Event() {
  for (int index = 0; index < kTouchEventLimit; ++index) {
    char path[32]{};
    std::snprintf(path, sizeof(path), "/dev/input/event%d", index);
    const int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) continue;
    char name[128]{};
    if (ioctl(fd, EVIOCGNAME(sizeof(name)), name) >= 0 && IsGt911Name(name))
      return fd;
    close(fd);
  }
  return -1;
}

const char* FindTextFont() {
  if (access(kPrimaryFont, R_OK) == 0) return kPrimaryFont;
  return access(kFallbackFont, R_OK) == 0 ? kFallbackFont : nullptr;
}

enum class TouchBackend : std::uint8_t { kNone, kEvent, kI2c };

}  // namespace

struct DeviceUi::Impl final {
  int spi{-1};
  int touch{-1};
  int dc{-1};
  int reset{-1};
  int backlight{-1};
  TouchBackend touch_backend{TouchBackend::kNone};

  int slot{0};
  int pointer_x{0};
  int pointer_y{0};
  bool pointer_active{false};

  std::atomic<bool> ready{false};
  std::atomic<bool> stop{false};
  std::atomic<bool> display_failed{false};
  std::mutex mutex;
  std::condition_variable wake;
  std::thread worker;
  bool worker_started{false};
  bool worker_succeeded{false};
  bool state_dirty{true};
  bool text_dirty{true};
  bool volume_dirty{true};
  bool camera_requested{false};
  bool camera_request_dirty{false};
  bool camera_frame_dirty{false};
  DeviceUiState state{DeviceUiState::kIdle};
  std::uint8_t volume_percent{60U};
  std::uint8_t pending_volume_percent{60U};
  bool volume_change_pending{false};
  std::uint8_t settings_volume_percent{60U};
  bool settings_save_pending{false};
  std::array<std::array<char, 64>, 2> lines{};
  std::array<DeviceUiAction, kActionQueueCapacity> actions{};
  std::size_t action_head{0U};
  std::size_t action_count{0U};
  std::array<std::uint16_t, kCameraPixelCount> camera_frame{};
  std::atomic<bool> camera_stop{false};
  std::atomic<pid_t> camera_process{-1};
  std::thread camera_worker;

  LvglScreen screen;
  lv_disp_draw_buf_t draw_buffer{};
  lv_disp_drv_t display_driver{};
  lv_indev_drv_t input_driver{};
  lv_disp_t* display{nullptr};
  lv_indev_t* input{nullptr};
  std::array<lv_color_t, kLogicalWidth * kDrawBufferRows> draw_pixels{};

  bool Command(std::uint8_t command, const std::uint8_t* data = nullptr,
               std::size_t bytes = 0U) {
    if (!SetGpio(dc, false) || !WriteAll(spi, &command, 1U)) return false;
    return bytes == 0U || (SetGpio(dc, true) && WriteAll(spi, data, bytes));
  }

  bool SetWindow(int column_start, int row_start, int column_end, int row_end) {
    const std::uint8_t columns[] = {
        static_cast<std::uint8_t>(column_start >> 8),
        static_cast<std::uint8_t>(column_start),
        static_cast<std::uint8_t>(column_end >> 8),
        static_cast<std::uint8_t>(column_end)};
    const std::uint8_t rows[] = {
        static_cast<std::uint8_t>(row_start >> 8),
        static_cast<std::uint8_t>(row_start),
        static_cast<std::uint8_t>(row_end >> 8),
        static_cast<std::uint8_t>(row_end)};
    return Command(0x2A, columns, sizeof(columns)) &&
           Command(0x2B, rows, sizeof(rows)) && Command(0x2C) &&
           SetGpio(dc, true);
  }

  void FailDisplay() {
    display_failed.store(true);
    ready.store(false);
    stop.store(true);
    wake.notify_all();
  }

  // ST7789 保持已验证的竖屏寄存器配置，仅在 flush 时把 LVGL 横屏脏区旋转到面板扫描顺序。
  static void FlushDisplay(lv_disp_drv_t* driver, const lv_area_t* area,
                           lv_color_t* pixels) {
    auto* self = static_cast<Impl*>(driver->user_data);
    bool ok = self != nullptr && area != nullptr && pixels != nullptr;
    if (ok) {
      const int x1 = std::max(0, static_cast<int>(area->x1));
      const int y1 = std::max(0, static_cast<int>(area->y1));
      const int x2 = std::min(kLogicalWidth - 1, static_cast<int>(area->x2));
      const int y2 = std::min(kLogicalHeight - 1, static_cast<int>(area->y2));
      const int source_width = static_cast<int>(area->x2) -
                               static_cast<int>(area->x1) + 1;
      if (x1 <= x2 && y1 <= y2 && source_width > 0) {
        ok = self->SetWindow(y1, kPanelHeight - 1 - x2, y2,
                             kPanelHeight - 1 - x1);
        std::array<std::uint8_t, 4096> block{};
        std::size_t used = 0U;
        for (int logical_x = x2; ok && logical_x >= x1; --logical_x) {
          for (int logical_y = y1; ok && logical_y <= y2; ++logical_y) {
            const std::size_t source_index = static_cast<std::size_t>(
                (logical_y - static_cast<int>(area->y1)) * source_width +
                logical_x - static_cast<int>(area->x1));
            const std::uint16_t pixel = pixels[source_index].full;
            block[used++] = static_cast<std::uint8_t>(pixel >> 8U);
            block[used++] = static_cast<std::uint8_t>(pixel);
            if (used == block.size()) {
              ok = WriteAll(self->spi, block.data(), used);
              used = 0U;
            }
          }
        }
        if (ok && used != 0U) ok = WriteAll(self->spi, block.data(), used);
      }
    }
    if (!ok && self != nullptr) self->FailDisplay();
    lv_disp_flush_ready(driver);
  }

  void QueueAction(DeviceUiAction action) {
    std::lock_guard<std::mutex> lock(mutex);
    if (action_count == actions.size()) {
      action_head = (action_head + 1U) % actions.size();
      --action_count;
    }
    const std::size_t tail = (action_head + action_count) % actions.size();
    actions[tail] = action;
    ++action_count;
  }

  void QueueVolumeChange(std::uint8_t percent, bool committed) {
    std::lock_guard<std::mutex> lock(mutex);
    // Dragging may emit one event per visual frame.  Keep only the newest
    // preview so wake/interrupt actions cannot be displaced by slider noise.
    pending_volume_percent = std::min<std::uint8_t>(percent, 100U);
    volume_change_pending = true;
    if (committed) {
      // 单槽只保留最后一次提交，拖动过程不会反复写 flash。
      settings_volume_percent = pending_volume_percent;
      settings_save_pending = true;
    }
  }

  void SavePendingVolumeSetting() {
    std::uint8_t percent = 0U;
    {
      std::lock_guard<std::mutex> lock(mutex);
      if (!settings_save_pending) return;
      percent = settings_volume_percent;
      settings_save_pending = false;
    }

    char text[16]{};
    const int length = std::snprintf(text, sizeof(text), "%u\n",
                                     static_cast<unsigned>(percent));
    const int fd = open(kUiSettingsTemporary,
                        O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    const bool wrote = fd >= 0 && length > 0 &&
                       static_cast<std::size_t>(length) < sizeof(text) &&
                       WriteAll(fd, text, static_cast<std::size_t>(length));
    if (fd >= 0) close(fd);
    if (!wrote || std::rename(kUiSettingsTemporary, kUiSettings) != 0) {
      unlink(kUiSettingsTemporary);
      std::fprintf(stderr, "boompi-ui: volume setting save failed: %s\n",
                   std::strerror(errno));
    }
  }

  void BeginTouch() {
    pointer_active = true;
  }

  void EndTouch() {
    pointer_active = false;
  }

  void HandleEvent(const input_event& event) {
    if (event.type != EV_ABS) return;
    if (event.code == ABS_MT_SLOT) {
      slot = event.value;
      return;
    }
    if (slot != 0) return;
    if (event.code == ABS_MT_POSITION_X) {
      pointer_y = std::clamp(event.value, 0, kPanelWidth - 1);
    } else if (event.code == ABS_MT_POSITION_Y) {
      pointer_x = kPanelHeight - 1 -
                  std::clamp(event.value, 0, kPanelHeight - 1);
    } else if (event.code == ABS_MT_TRACKING_ID && event.value >= 0) {
      BeginTouch();
    } else if (event.code == ABS_MT_TRACKING_ID && event.value < 0) {
      EndTouch();
    }
  }

  bool ReadGt911(std::uint16_t address, std::uint8_t* data,
                 std::size_t bytes) {
    std::uint8_t register_address[] = {
        static_cast<std::uint8_t>(address >> 8U),
        static_cast<std::uint8_t>(address)};
    i2c_msg messages[2]{};
    messages[0].addr = kGt911Address;
    messages[0].len = sizeof(register_address);
    messages[0].buf = register_address;
    messages[1].addr = kGt911Address;
    messages[1].flags = I2C_M_RD;
    messages[1].len = static_cast<__u16>(bytes);
    messages[1].buf = data;
    i2c_rdwr_ioctl_data transfer{messages, 2U};
    return ioctl(touch, I2C_RDWR, &transfer) == 2;
  }

  bool ClearGt911Status() {
    std::uint8_t command[] = {
        static_cast<std::uint8_t>(kGt911StatusRegister >> 8U),
        static_cast<std::uint8_t>(kGt911StatusRegister), 0U};
    i2c_msg message{};
    message.addr = kGt911Address;
    message.len = sizeof(command);
    message.buf = command;
    i2c_rdwr_ioctl_data transfer{&message, 1U};
    return ioctl(touch, I2C_RDWR, &transfer) == 1;
  }

  void PollGt911() {
    std::uint8_t status = 0U;
    if (!ReadGt911(kGt911StatusRegister, &status, 1U) ||
        (status & 0x80U) == 0U)
      return;
    const unsigned count = status & 0x0FU;
    if (count == 0U) {
      EndTouch();
    } else if (count <= 5U) {
      std::array<std::uint8_t, 8> point{};
      if (ReadGt911(kGt911PointRegister, point.data(), point.size())) {
        const int raw_x = point[1] | (static_cast<int>(point[2]) << 8);
        const int raw_y = point[3] | (static_cast<int>(point[4]) << 8);
        if (!pointer_active) BeginTouch();
        pointer_y = std::clamp(raw_x, 0, kPanelWidth - 1);
        pointer_x = kPanelHeight - 1 -
                    std::clamp(raw_y, 0, kPanelHeight - 1);
      }
    }
    ClearGt911Status();
  }

  void ReadEventInput(lv_indev_data_t* data) {
    input_event event{};
    while (read(touch, &event, sizeof(event)) == sizeof(event)) {
      HandleEvent(event);
      if (event.type == EV_SYN && event.code == SYN_REPORT) {
        data->point.x = static_cast<lv_coord_t>(pointer_x);
        data->point.y = static_cast<lv_coord_t>(pointer_y);
        data->state = pointer_active ? LV_INDEV_STATE_PR : LV_INDEV_STATE_REL;
        data->continue_reading = true;
        return;
      }
    }
  }

  static void ReadInput(lv_indev_drv_t* driver, lv_indev_data_t* data) {
    auto* self = static_cast<Impl*>(driver->user_data);
    data->continue_reading = false;
    if (self == nullptr) {
      data->state = LV_INDEV_STATE_REL;
      return;
    }
    if (self->touch_backend == TouchBackend::kEvent)
      self->ReadEventInput(data);
    else if (self->touch_backend == TouchBackend::kI2c)
      self->PollGt911();
    data->point.x = static_cast<lv_coord_t>(self->pointer_x);
    data->point.y = static_cast<lv_coord_t>(self->pointer_y);
    data->state = self->pointer_active ? LV_INDEV_STATE_PR : LV_INDEV_STATE_REL;
  }

  static void VoiceInterrupted(void* user_data) {
    static_cast<Impl*>(user_data)->QueueAction(DeviceUiAction::kInterrupt);
  }

  static void AppAction(std::size_t index, void* user_data) {
    if (index == 0U)
      static_cast<Impl*>(user_data)->QueueAction(DeviceUiAction::kWake);
  }

  static void CameraActivity(bool active, void* user_data) {
    auto* self = static_cast<Impl*>(user_data);
    {
      std::lock_guard<std::mutex> lock(self->mutex);
      if (self->camera_requested == active) return;
      self->camera_requested = active;
      self->camera_request_dirty = true;
    }
    self->wake.notify_one();
  }

  static void VolumeChanged(std::uint8_t percent,
                            LvglScreen::VolumeChangePhase phase,
                            void* user_data) {
    auto* self = static_cast<Impl*>(user_data);
    if (phase == LvglScreen::VolumeChangePhase::kBegin) return;
    self->QueueVolumeChange(
        percent, phase == LvglScreen::VolumeChangePhase::kCommit);
  }

  void CaptureCamera() {
    int output[2]{};
    if (pipe(output) != 0) {
      std::fprintf(stderr, "boompi-ui: camera pipe failed: %s\n",
                   std::strerror(errno));
      return;
    }
    const pid_t child = fork();
    if (child == 0) {
      close(output[0]);
      setpgid(0, 0);
      if (dup2(output[1], STDOUT_FILENO) < 0) _exit(126);
      close(output[1]);
      execl("/bin/sh", "sh", "-c", kCameraPipeline,
            static_cast<char*>(nullptr));
      _exit(127);
    }
    close(output[1]);
    if (child < 0) {
      close(output[0]);
      std::fprintf(stderr, "boompi-ui: camera fork failed: %s\n",
                   std::strerror(errno));
      return;
    }

    setpgid(child, child);
    camera_process.store(child);
    std::fprintf(stderr, "boompi-ui: camera preview started; pid=%d\n",
                 static_cast<int>(child));
    std::array<std::uint8_t, kCameraFrameBytes> frame{};
    while (!camera_stop.load()) {
      std::size_t offset = 0U;
      while (offset < frame.size() && !camera_stop.load()) {
        const ssize_t count = read(output[0], frame.data() + offset,
                                   frame.size() - offset);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) {
          offset = 0U;
          break;
        }
        offset += static_cast<std::size_t>(count);
      }
      if (offset != frame.size()) break;
      {
        std::lock_guard<std::mutex> lock(mutex);
        std::memcpy(camera_frame.data(), frame.data(), frame.size());
        camera_frame_dirty = true;
      }
      wake.notify_one();
    }
    close(output[0]);
    if (kill(-child, SIGTERM) != 0 && errno != ESRCH) {
      std::fprintf(stderr, "boompi-ui: camera stop signal failed: %s\n",
                   std::strerror(errno));
    }
    int status = 0;
    while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
    camera_process.store(-1);
    std::fprintf(stderr, "boompi-ui: camera preview stopped\n");
  }

  void StartCamera() {
    if (camera_worker.joinable()) camera_worker.join();
    camera_stop.store(false);
    try {
      camera_worker = std::thread([this] { CaptureCamera(); });
    } catch (...) {
      std::fprintf(stderr, "boompi-ui: camera worker could not start\n");
    }
  }

  void StopCamera() {
    camera_stop.store(true);
    const pid_t child = camera_process.load();
    if (child > 0) {
      kill(-child, SIGTERM);
      usleep(100000);
      if (camera_process.load() == child) kill(-child, SIGKILL);
    }
    if (camera_worker.joinable()) camera_worker.join();
    camera_process.store(-1);
  }

  bool InitializeLvgl() {
    lv_init();
    lv_disp_draw_buf_init(&draw_buffer, draw_pixels.data(), nullptr,
                          static_cast<std::uint32_t>(draw_pixels.size()));
    lv_disp_drv_init(&display_driver);
    display_driver.hor_res = kLogicalWidth;
    display_driver.ver_res = kLogicalHeight;
    display_driver.flush_cb = FlushDisplay;
    display_driver.draw_buf = &draw_buffer;
    display_driver.user_data = this;
    display = lv_disp_drv_register(&display_driver);
    if (display == nullptr) return false;

    lv_indev_drv_init(&input_driver);
    input_driver.type = LV_INDEV_TYPE_POINTER;
    input_driver.read_cb = ReadInput;
    input_driver.user_data = this;
    input = lv_indev_drv_register(&input_driver);
    if (input == nullptr) return false;

    const char* text_font = FindTextFont();
    if (text_font == nullptr || access(kIconFont, R_OK) != 0 ||
        !screen.Create(text_font, kIconFont))
      return false;
    screen.SetVoiceInterruptHandler(VoiceInterrupted, this);
    screen.SetAppActionHandler(AppAction, this);
    screen.SetCameraActivityHandler(CameraActivity, this);
    screen.SetVolumeChangeHandler(VolumeChanged, this);
    return true;
  }

  void ShutdownLvgl() {
    screen.Destroy();
    if (input != nullptr) {
      lv_indev_delete(input);
      input = nullptr;
    }
    if (display != nullptr) {
      lv_disp_remove(display);
      display = nullptr;
    }
  }

  void SignalStartup(bool succeeded) {
    {
      std::lock_guard<std::mutex> lock(mutex);
      worker_started = true;
      worker_succeeded = succeeded;
      ready.store(succeeded);
    }
    wake.notify_all();
  }

  // LVGL、触摸和 SPI 刷屏只由本线程调用，业务线程只交换模型快照和有界动作队列。
  void Run() {
    // RV1106 is single-core.  Keep display animation below the audio actor so
    // an SPI refresh cannot consume the complete ALSA capture safety window.
    static_cast<void>(nice(5));
    const bool initialized = InitializeLvgl();
    if (!initialized) {
      ShutdownLvgl();
      SignalStartup(false);
      return;
    }

    DeviceUiState shown{};
    std::uint8_t shown_volume = 60U;
    std::array<std::array<char, 64>, 2> text{};
    {
      std::lock_guard<std::mutex> lock(mutex);
      shown = state;
      shown_volume = volume_percent;
      text = lines;
      state_dirty = false;
      text_dirty = false;
      volume_dirty = false;
    }
    screen.SetState(shown);
    screen.SetText(text[0].data(), text[1].data());
    screen.SetVolume(shown_volume);
    SignalStartup(true);

    auto previous = std::chrono::steady_clock::now();
    while (!stop.load()) {
      const auto now = std::chrono::steady_clock::now();
      const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
          now - previous).count();
      previous = now;
      if (elapsed > 0) {
        lv_tick_inc(static_cast<std::uint32_t>(
            std::min<std::int64_t>(elapsed, UINT32_MAX)));
      }

      bool update_state = false;
      bool update_text = false;
      bool update_volume = false;
      bool change_camera = false;
      bool start_camera = false;
      bool update_camera_frame = false;
      std::array<std::uint16_t, kCameraPixelCount> camera_pixels{};
      {
        std::lock_guard<std::mutex> lock(mutex);
        update_state = state_dirty;
        update_text = text_dirty;
        update_volume = volume_dirty;
        change_camera = camera_request_dirty;
        start_camera = camera_requested;
        update_camera_frame = camera_frame_dirty;
        if (update_state) shown = state;
        if (update_text) text = lines;
        if (update_volume) shown_volume = volume_percent;
        if (update_camera_frame) camera_pixels = camera_frame;
        state_dirty = false;
        text_dirty = false;
        volume_dirty = false;
        camera_request_dirty = false;
        camera_frame_dirty = false;
      }
      if (change_camera) {
        if (start_camera)
          StartCamera();
        else
          StopCamera();
      }
      if (update_state) screen.SetState(shown);
      if (update_text) screen.SetText(text[0].data(), text[1].data());
      if (update_volume) screen.SetVolume(shown_volume);
      if (update_camera_frame && start_camera)
        screen.SetCameraFrame(camera_pixels.data(), camera_pixels.size());

      static_cast<void>(lv_timer_handler());
      SavePendingVolumeSetting();
      if (display_failed.load()) break;
      std::unique_lock<std::mutex> lock(mutex);
      // Subtitle deltas coalesce until the next visual frame.  State/page and
      // camera lifecycle changes still wake immediately.
      wake.wait_for(lock, kUiRefreshPeriod, [this] {
        return stop.load() || state_dirty ||
               volume_dirty || camera_request_dirty || camera_frame_dirty;
      });
    }

    StopCamera();
    ShutdownLvgl();
    ready.store(false);
  }
};

DeviceUi::~DeviceUi() noexcept { Close(); }

std::uint8_t DeviceUi::LoadVolume(const std::uint8_t fallback) noexcept {
  unsigned saved = 0U;
  std::FILE* input = std::fopen(kUiSettings, "r");
  if (input == nullptr) return std::min<std::uint8_t>(fallback, 100U);
  const bool valid = std::fscanf(input, "%u", &saved) == 1 && saved <= 100U;
  std::fclose(input);
  return valid ? static_cast<std::uint8_t>(saved)
               : std::min<std::uint8_t>(fallback, 100U);
}

bool DeviceUi::Open() noexcept {
  Close();
  impl_ = new (std::nothrow) Impl;
  if (impl_ == nullptr) return false;

  impl_->spi = open("/dev/spidev0.0", O_RDWR | O_CLOEXEC);
  std::uint8_t mode = 0U;
  std::uint8_t bits = 8U;
  std::uint32_t speed = kSpiSpeedHz;
  if (impl_->spi < 0 || ioctl(impl_->spi, SPI_IOC_WR_MODE, &mode) < 0 ||
      ioctl(impl_->spi, SPI_IOC_WR_BITS_PER_WORD, &bits) < 0 ||
      ioctl(impl_->spi, SPI_IOC_WR_MAX_SPEED_HZ, &speed) < 0 ||
      ioctl(impl_->spi, SPI_IOC_RD_MAX_SPEED_HZ, &speed) < 0) {
    Close();
    return false;
  }
  std::fprintf(stderr, "boompi-ui: SPI speed=%u Hz; refresh=30 FPS\n", speed);
  impl_->dc = OpenGpio(kGpioRoot, kDcGpio);
  impl_->reset = OpenGpio(kGpioRoot, kResetGpio);
  impl_->backlight = OpenGpio(kGpioRoot, kBacklightGpio);
  if (impl_->dc < 0 || impl_->reset < 0 || impl_->backlight < 0) {
    Close();
    return false;
  }

  SetGpio(impl_->backlight, false);
  SetGpio(impl_->reset, false);
  usleep(100000);
  if (!SetGpio(impl_->reset, true)) {
    Close();
    return false;
  }
  usleep(100000);
  static constexpr std::uint8_t init[] = {
      0xB2, 5, 0x0C, 0x0C, 0,    0x33, 0x33, 0x36, 1, 0,    0x3A, 1,
      0x55, 0xB7, 1,    0x55, 0xBB, 1,    0x1A, 0xC0, 1,    0x2C, 0xC2,
      1,    1,    0xC3, 1,    0x19, 0xC6, 1,    0x0F, 0xD0, 1,    0xA7,
      0xD0, 2,    0xA4, 0xA1, 0xD6, 1,    0xA1, 0xE0, 14,   0xF0, 3,
      9,    0x0B, 0x0A, 0x16, 0x2B, 0x33, 0x41, 0x38, 0x14, 0x14, 0x29,
      0x2F, 0xE1, 14,   0xF0, 4,    6,    9,    8,    4,    0x2B, 0x32,
      0x41, 0x36, 0x12, 0x12, 0x2A, 0x30, 0x21, 0,    0x2A, 4,    0,
      0,    0,    0xEF, 0x2B, 4,    0,    0,    1,    0x3F};
  for (std::size_t offset = 0U; offset < sizeof(init);) {
    const std::uint8_t command = init[offset++];
    const std::uint8_t length = init[offset++];
    if (!impl_->Command(command, init + offset, length)) {
      Close();
      return false;
    }
    offset += length;
  }
  if (!impl_->Command(0x11)) {
    Close();
    return false;
  }
  usleep(120000);
  if (!impl_->Command(0x29) || !SetGpio(impl_->backlight, true)) {
    Close();
    return false;
  }

  impl_->touch = OpenGt911Event();
  if (impl_->touch >= 0) {
    impl_->touch_backend = TouchBackend::kEvent;
  } else {
    if (!ResetGt911AtAddress14()) {
      Close();
      return false;
    }
    impl_->touch = open(kGt911I2cDevice, O_RDWR | O_CLOEXEC);
    if (impl_->touch >= 0 &&
        ioctl(impl_->touch, I2C_SLAVE, kGt911Address) >= 0) {
      impl_->touch_backend = TouchBackend::kI2c;
    } else {
      Close();
      return false;
    }
  }

  try {
    impl_->worker = std::thread([state = impl_] { state->Run(); });
  } catch (...) {
    Close();
    return false;
  }
  std::unique_lock<std::mutex> lock(impl_->mutex);
  const bool started = impl_->wake.wait_for(lock, kUiStartTimeout, [this] {
    return impl_->worker_started;
  });
  const bool succeeded = started && impl_->worker_succeeded;
  lock.unlock();
  if (!succeeded) {
    Close();
    return false;
  }
  return true;
}

void DeviceUi::SetState(DeviceUiState state) noexcept {
  if (impl_ == nullptr || !impl_->ready.load()) return;
  std::lock_guard<std::mutex> lock(impl_->mutex);
  if (impl_->state == state) return;
  impl_->state = state;
  impl_->state_dirty = true;
  impl_->wake.notify_one();
}

void DeviceUi::SetText(std::string_view first, std::string_view second) noexcept {
  if (impl_ == nullptr || !impl_->ready.load()) return;
  std::array<std::array<char, 64>, 2> updated{};
  const std::string_view source[] = {first, second};
  for (int line = 0; line < 2; ++line) {
    const std::size_t length =
        std::min(source[line].size(), updated[line].size() - 1U);
    if (length != 0U)
      std::memcpy(updated[line].data(), source[line].data(), length);
  }
  std::lock_guard<std::mutex> lock(impl_->mutex);
  if (impl_->lines == updated) return;
  impl_->lines = updated;
  impl_->text_dirty = true;
  impl_->wake.notify_one();
}

bool DeviceUi::PollAction(DeviceUiAction* action) noexcept {
  if (action == nullptr || impl_ == nullptr) return false;
  std::lock_guard<std::mutex> lock(impl_->mutex);
  if (impl_->action_count == 0U) return false;
  *action = impl_->actions[impl_->action_head];
  impl_->action_head = (impl_->action_head + 1U) % impl_->actions.size();
  --impl_->action_count;
  return true;
}

bool DeviceUi::PollVolumeChange(std::uint8_t* percent) noexcept {
  if (percent == nullptr || impl_ == nullptr) return false;
  std::lock_guard<std::mutex> lock(impl_->mutex);
  if (!impl_->volume_change_pending) return false;
  *percent = impl_->pending_volume_percent;
  impl_->volume_change_pending = false;
  return true;
}

void DeviceUi::SetVolume(const std::uint8_t percent) noexcept {
  if (impl_ == nullptr || !impl_->ready.load()) return;
  std::lock_guard<std::mutex> lock(impl_->mutex);
  const std::uint8_t limited = std::min<std::uint8_t>(percent, 100U);
  if (impl_->volume_percent == limited) return;
  impl_->volume_percent = limited;
  impl_->volume_dirty = true;
  impl_->wake.notify_one();
}

void DeviceUi::Close() noexcept {
  if (impl_ == nullptr) return;
  impl_->ready.store(false);
  impl_->stop.store(true);
  impl_->wake.notify_all();
  if (impl_->worker.joinable()) impl_->worker.join();
  SetGpio(impl_->backlight, false);
  for (int fd : {impl_->touch, impl_->spi, impl_->dc, impl_->reset,
                 impl_->backlight}) {
    if (fd >= 0) close(fd);
  }
  delete impl_;
  impl_ = nullptr;
}

}  // namespace boompi::ui
