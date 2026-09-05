#pragma once
#include "boompi/ui/ui_view.h"

namespace boompi::ui {
// UI worker 独占 LVGL/触摸；camera worker 仅交付固定尺寸像素。
class DeviceUi final {
 public:
  DeviceUi() noexcept = default;
  ~DeviceUi() noexcept;
  DeviceUi(const DeviceUi&) = delete;
  DeviceUi& operator=(const DeviceUi&) = delete;
  static std::uint8_t LoadVolume(std::uint8_t fallback = 60) noexcept;
  bool Open();
  void Show(const UiView& view) noexcept;
  bool Poll(UiAction* action) noexcept;
  void Close() noexcept;
 private:
  struct Impl;
  Impl* impl_{nullptr};
};
}  // namespace boompi::ui
