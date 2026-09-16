#pragma once

#include "boompi/ui/ui_view.h"

namespace boompi::ui::page {
enum class Event { Wake, Interrupt, Volume, SaveVolume };
using Handler = void (*)(Event, std::uint8_t);
// open/close与运行期不重叠；handler由UI模块提供，其余调用均在UI线程。
bool open(const char* font_path, Handler handler);
void show(const UiView& view);
void close() noexcept;
}  // namespace boompi::ui::page
