#pragma once
#include "boompi/ui/ui_view.h"

namespace boompi::ui {
// 应用串行管理生命周期；运行期仅UI线程调用LVGL。
std::uint8_t load_volume(std::uint8_t fallback = 60) noexcept;
bool open();
void show(const UiView& view) noexcept;
bool poll_action(UiAction& action) noexcept;
void close() noexcept;
}  // namespace boompi::ui
