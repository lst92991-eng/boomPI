#pragma once
#include <array>
#include "boompi/ui/ui_view.h"

namespace boompi::ui::page {
inline constexpr std::size_t kWidth = 320, kHeight = 180;
using Image = std::array<std::uint16_t, kWidth * kHeight>;
enum class Event { Wake, Interrupt, Volume, SaveVolume, CameraOn, CameraOff };
using Handler = void (*)(Event, std::uint8_t);
// open/close与运行期不重叠；其余调用和回调都在唯一的LVGL线程。
bool open(const char* font_path, Handler handler = nullptr);
void show(const UiView& view);
void camera(bool visible);
Image& pixels() noexcept;  // UI自己的稳定缓冲；采集线程不得直接写入。
void present(CameraStatus status, bool new_frame);
void close() noexcept;
}  // namespace boompi::ui::page
