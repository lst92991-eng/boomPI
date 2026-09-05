/** @file display_touch.cpp
 * @brief ST7789P3 横屏刷写和 GT911 触摸。板级引脚、寄存器与恢复时序集中在此。
 */
#include "display_touch.h"

#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <linux/i2c.h>
#include <linux/spi/spidev.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <string>

namespace boompi::platform::rv1106 {
namespace {

// 面板原生方向为 240x320，产品界面按 320x240 横屏绘制；刷屏和触摸都在端口层旋转。
constexpr int kPanelWidth = 240, kPanelHeight = 320;
// GPIO 编号来自当前 RV1106 板级连线，修改硬件版本时需要与 DTS 和网表一同核对。
constexpr int kBacklightGpio = 53, kDataCommandGpio = 66, kPanelResetGpio = 67;
constexpr int kTouchResetGpio = 64, kTouchInterruptGpio = 73;
constexpr int kGt911Address = 0x14;
constexpr std::uint16_t kTouchStatus = 0x814E;
constexpr std::uint16_t kTouchPoint = 0x814F;
constexpr unsigned kTouchFailureLimit = 3U;
constexpr unsigned kTouchRecoveryLimit = 2U;
// spidev 接收的是上限请求，Open() 还会回读驱动实际接受的频率并记录到日志。
constexpr unsigned kSpiSpeedHz = 80000000U;
constexpr char kGpioRoot[] = "/sys/class/gpio";

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

bool WriteText(const std::string& path, const std::string& text, int flags = O_WRONLY) {
  const int fd = open(path.c_str(), flags | O_CLOEXEC, 0644);
  if (fd < 0) {
    return false;
  }
  const bool ok = WriteAll(fd, text.data(), text.size());
  close(fd);
  return ok;
}

/**
 * @brief 通过旧 sysfs GPIO ABI 导出输出脚并保持 value fd 打开。
 *
 * export 后节点由内核异步创建，因此等待窗口限制为约 500 ms。返回的 fd 由
 * DisplayTouch::Close() 统一释放，GPIO 本身保留导出状态供下一次启动复用。
 */
int OpenGpio(int number) {
  const std::string base = std::string(kGpioRoot) + "/gpio" + std::to_string(number);
  if (access(base.c_str(), F_OK) != 0) {
    if (!WriteText(std::string(kGpioRoot) + "/export", std::to_string(number))) {
      return -1;
    }
    for (int retry = 0; retry < 50 && access(base.c_str(), F_OK) != 0; ++retry) {
      usleep(10000);
    }
  }
  if (!WriteText(base + "/direction", "out")) {
    return -1;
  }
  return open((base + "/value").c_str(), O_WRONLY | O_CLOEXEC);
}

bool SetGpio(int fd, bool high) {
  const char value = high ? '1' : '0';
  return fd >= 0 && lseek(fd, 0, SEEK_SET) == 0 && WriteAll(fd, &value, 1U);
}

}  // namespace

bool DisplayTouch::Open() {
  Close();
  spi = open("/dev/spidev0.0", O_RDWR | O_CLOEXEC);
  std::uint8_t mode = 0U;
  std::uint8_t bits = 8U;
  std::uint32_t speed = kSpiSpeedHz;
  const bool ready = spi >= 0 && ioctl(spi, SPI_IOC_WR_MODE, &mode) == 0 &&
                     ioctl(spi, SPI_IOC_WR_BITS_PER_WORD, &bits) == 0 &&
                     ioctl(spi, SPI_IOC_WR_MAX_SPEED_HZ, &speed) == 0 &&
                     ioctl(spi, SPI_IOC_RD_MAX_SPEED_HZ, &speed) == 0 && InitPanel() &&
                     InitTouch();
  if (!ready) {
    Close();
    return false;
  }
  std::fprintf(stderr, "boompi-ui: SPI=%u Hz; touch=GT911/i2c-3\n", speed);
  return true;
}

void DisplayTouch::Close() noexcept {
  SetGpio(backlight, false);
  const int descriptors[] = {touch, spi, data_command, panel_reset, backlight};
  for (int fd : descriptors) {
    if (fd >= 0) {
      close(fd);
    }
  }
  spi = touch = data_command = panel_reset = backlight = -1;
  pointer_x = pointer_y = 0;
  pointer_pressed = touch_disabled = false;
  touch_failures = touch_recovery_attempts = 0U;
  next_touch_recovery = {};
}

/**
 * @brief 按 ST7789P3 的 D/C 时序发送一条命令及可选 payload。
 *
 * D/C 拉低覆盖命令字节，payload 发送前再拉高；整个调用只在 UI worker 中发生，
 * 因而 GPIO 电平和 SPI 字节流不会被其他页面更新交叉打断。
 */
bool DisplayTouch::Command(std::uint8_t command, const std::uint8_t* data, std::size_t bytes) {
  if (!SetGpio(data_command, false) || !WriteAll(spi, &command, 1U)) {
    return false;
  }
  return bytes == 0U || (SetGpio(data_command, true) && WriteAll(spi, data, bytes));
}

/**
 * @brief 将 LVGL 横屏脏矩形旋转并刷入 ST7789P3 原生坐标。
 *
 * LVGL 的 RGB565 主机字节序像素在这里转成面板要求的高字节在前，并使用固定
 * 4 KiB 暂存块限制栈和单次 write 大小。刷屏失败时熄灭背光并返回 false。
 * LVGL 的 flush_ready 由 DeviceUi 回调统一执行，避免库内部一直等待。
 */
bool DisplayTouch::Flush(const lv_area_t& area, const lv_color_t* pixels) {
  const int width = area.x2 - area.x1 + 1;
  const int x1 = area.y1, x2 = area.y2;
  const int y1 = kPanelHeight - 1 - area.x2, y2 = kPanelHeight - 1 - area.x1;
  const std::uint8_t columns[] = {
      static_cast<std::uint8_t>(x1 >> 8), static_cast<std::uint8_t>(x1),
      static_cast<std::uint8_t>(x2 >> 8), static_cast<std::uint8_t>(x2)};
  const std::uint8_t rows[] = {
      static_cast<std::uint8_t>(y1 >> 8), static_cast<std::uint8_t>(y1),
      static_cast<std::uint8_t>(y2 >> 8), static_cast<std::uint8_t>(y2)};
  bool ok = Command(0x2A, columns, sizeof(columns)) && Command(0x2B, rows, sizeof(rows)) &&
            Command(0x2C) && SetGpio(data_command, true);
  std::size_t used = 0U;
  for (int x = area.x2; ok && x >= area.x1; --x) {
    for (int y = area.y1; y <= area.y2; ++y) {
      const auto pixel = pixels[(y - area.y1) * width + x - area.x1].full;
      flush_pixels[used++] = static_cast<std::uint8_t>(pixel >> 8U);
      flush_pixels[used++] = static_cast<std::uint8_t>(pixel);
      if (used == flush_pixels.size()) {
        ok = WriteAll(spi, flush_pixels.data(), used);
        used = 0U;
        if (!ok) {
          break;
        }
      }
    }
  }
  if (ok && used != 0U) {
    ok = WriteAll(spi, flush_pixels.data(), used);
  }
  if (!ok) {
    SetGpio(backlight, false);
  }
  return ok;
}

/**
 * @brief 按上电时序复位并初始化 ST7789P3 面板。
 *
 * 背光先保持关闭，避免复位和寄存器配置期间显示随机显存；RESET 的两个 100 ms
 * 窗口以及 Sleep Out 后的 120 ms 来自控制器上电要求。只有 Display On 成功后才
 * 点亮背光，因此 Open() 的成功意味着用户能够看到完整初始化后的页面。
 */
bool DisplayTouch::InitPanel() {
  if ((data_command = OpenGpio(kDataCommandGpio)) < 0 ||
      (panel_reset = OpenGpio(kPanelResetGpio)) < 0 ||
      (backlight = OpenGpio(kBacklightGpio)) < 0) {
    return false;
  }
  if (!SetGpio(backlight, false) || !SetGpio(panel_reset, false)) {
    return false;
  }
  usleep(100000);
  if (!SetGpio(panel_reset, true)) {
    return false;
  }
  usleep(100000);
  // 初始化表绑定当前面板模组；每组数据依次为 command、payload size、payload。
  // 维护时应对照模组厂商序列整体更新，单独猜测 gamma 或电源寄存器会产生隐蔽色偏。
  static constexpr std::uint8_t init[] = {
      0xB2, 5,    0x0C, 0x0C, 0,    0x33, 0x33, 0x36, 1,    0,    0x3A, 1,    0x55, 0xB7, 1,
      0x55, 0xBB, 1,    0x1A, 0xC0, 1,    0x2C, 0xC2, 1,    1,    0xC3, 1,    0x19, 0xC6, 1,
      0x0F, 0xD0, 1,    0xA7, 0xD0, 2,    0xA4, 0xA1, 0xD6, 1,    0xA1, 0xE0, 14,   0xF0, 3,
      9,    0x0B, 0x0A, 0x16, 0x2B, 0x33, 0x41, 0x38, 0x14, 0x14, 0x29, 0x2F, 0xE1, 14,   0xF0,
      4,    6,    9,    8,    4,    0x2B, 0x32, 0x41, 0x36, 0x12, 0x12, 0x2A, 0x30, 0x21, 0,
      0x2A, 4,    0,    0,    0,    0xEF, 0x2B, 4,    0,    0,    1,    0x3F};
  for (std::size_t at = 0U; at < sizeof(init);) {
    const std::uint8_t size = init[at + 1U];
    if (!Command(init[at], init + at + 2U, size)) {
      return false;
    }
    at += size + 2U;
  }
  if (!Command(0x11)) {
    return false;
  }
  usleep(120000);
  return Command(0x29) && SetGpio(backlight, true);
}

/**
 * @brief 复位 GT911、锁定 0x14 地址并打开 I2C-3。
 *
 * GT911 在 RESET 上升沿采样 INT 电平；这里用 INT=1 选择 0x14，随后把该 GPIO
 * 恢复为输入。初始化结束前先试读状态寄存器，避免把线路故障延迟到 LVGL 循环。
 */
bool DisplayTouch::InitTouch() {
  const int touch_reset = OpenGpio(kTouchResetGpio), interrupt = OpenGpio(kTouchInterruptGpio);
  if (touch_reset < 0 || interrupt < 0) {
    if (touch_reset >= 0) {
      close(touch_reset);
    }
    if (interrupt >= 0) {
      close(interrupt);
    }
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
  if (!reset_ok) {
    return false;
  }
  const std::string irq =
      std::string(kGpioRoot) + "/gpio" + std::to_string(kTouchInterruptGpio) + "/direction";
  if (!WriteText(irq, "in")) {
    return false;
  }
  usleep(60000);
  touch = open("/dev/i2c-3", O_RDWR | O_CLOEXEC);
  std::uint8_t status = 0U;
  if (touch >= 0 && ReadTouch(kTouchStatus, &status, 1U)) {
    return true;
  }
  if (touch >= 0) {
    close(touch);
  }
  touch = -1;
  return false;
}

/**
 * @brief 使用一次 I2C_RDWR combined transfer 读取 GT911 寄存器。
 *
 * 寄存器地址为大端 16 位，write/read 两条 message 之间保持 repeated-start，避免
 * STOP 让控制器丢失当前地址。调用方与恢复逻辑都属于 UI worker，无需额外锁。
 */
bool DisplayTouch::ReadTouch(std::uint16_t address, std::uint8_t* data, std::size_t size) {
  std::uint8_t reg[] = {static_cast<std::uint8_t>(address >> 8U),
                        static_cast<std::uint8_t>(address)};
  i2c_msg messages[] = {{kGt911Address, 0, 2, reg},
                        {kGt911Address, I2C_M_RD, static_cast<__u16>(size), data}};
  i2c_rdwr_ioctl_data transfer{messages, 2U};
  return ioctl(touch, I2C_RDWR, &transfer) == 2;
}

/** @brief 写零确认本轮触摸数据，使 GT911 可以发布下一组坐标。 */
bool DisplayTouch::ClearTouchStatus() {
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
void DisplayTouch::TouchFailed(const char* stage) {
  pointer_pressed = false;
  if (touch_disabled) {
    return;
  }
  ++touch_failures;
  if (touch_failures < kTouchFailureLimit) {
    return;
  }

  const auto now = std::chrono::steady_clock::now();
  if (now < next_touch_recovery) {
    return;
  }
  if (touch_recovery_attempts >= kTouchRecoveryLimit) {
    touch_disabled = true;
    std::fprintf(stderr, "boompi-ui: GT911 disabled after bounded recovery\n");
    return;
  }

  ++touch_recovery_attempts;
  next_touch_recovery = now + std::chrono::seconds(2);
  std::fprintf(stderr, "boompi-ui: GT911 %s failed; recovery %u/%u\n", stage,
               touch_recovery_attempts, kTouchRecoveryLimit);
  if (touch >= 0) {
    close(touch);
  }
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
void DisplayTouch::PollTouch() {
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
  if (count == 0U) {
    pointer_pressed = false;
  } else if (count <= 5U) {
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

void DisplayTouch::ReadInput(lv_indev_data_t* data) {
  PollTouch();
  data->point.x = static_cast<lv_coord_t>(pointer_x);
  data->point.y = static_cast<lv_coord_t>(pointer_y);
  data->state = pointer_pressed ? LV_INDEV_STATE_PR : LV_INDEV_STATE_REL;
}

}  // namespace boompi::platform::rv1106
