/** @file display_touch.h
 * @brief ST7789P3 显示与 GT911 触摸的板端端口。
 */
#pragma once

#include <lvgl.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>

namespace boompi::platform::rv1106 {

/** 打开/关闭由 DeviceUi 控制；运行期只由 UI worker 调用，不创建线程。 */
class DisplayTouch final {
 public:
  DisplayTouch() noexcept = default;
  ~DisplayTouch() noexcept {
    Close();
  }
  DisplayTouch(const DisplayTouch&) = delete;
  DisplayTouch& operator=(const DisplayTouch&) = delete;

  bool Open();
  void Close() noexcept;
  /// 将逻辑横屏脏矩形旋转写入面板。失败会关背光，由 UI worker 决定退出。
  bool Flush(const lv_area_t& area, const lv_color_t* pixels);
  /// 返回逻辑横屏坐标；GT911 故障恢复保留在本端口内。
  void ReadInput(lv_indev_data_t* data);

 private:
  bool Command(std::uint8_t command, const std::uint8_t* data = nullptr,
               std::size_t bytes = 0U);
  bool InitPanel();
  bool InitTouch();
  bool ReadTouch(std::uint16_t address, std::uint8_t* data, std::size_t size);
  bool ClearTouchStatus();
  void TouchFailed(const char* stage);
  void PollTouch();

  int spi{-1}, touch{-1}, data_command{-1}, panel_reset{-1}, backlight{-1};
  int pointer_x{0}, pointer_y{0};
  bool pointer_pressed{false};
  unsigned touch_failures{0U}, touch_recovery_attempts{0U};
  bool touch_disabled{false};
  std::chrono::steady_clock::time_point next_touch_recovery{};
  std::array<std::uint8_t, 4096> flush_pixels{};
};

}  // namespace boompi::platform::rv1106
