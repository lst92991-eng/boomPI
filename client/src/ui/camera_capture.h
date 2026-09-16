#pragma once
#include "boompi/ui/lvgl_screen.h"

namespace boompi::ui::camera_capture {
inline constexpr unsigned kTargetFps = 5;
// UI线程管理open/read/close；工作线程独占管线和回收，只保存最新完整帧。
bool open();
bool read(page::Image& output, CameraStatus& status);
void close() noexcept;
}  // namespace boompi::ui::camera_capture
