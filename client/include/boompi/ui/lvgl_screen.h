/** @file lvgl_screen.h
 * @brief 小智单页控件及交互；负责显示应用快照、发出意图，不改变对话状态。
 */
#pragma once

#include "boompi/ui/ui_view.h"

namespace boompi::ui::page {
enum class Event { Wake, Interrupt, Volume, SaveVolume };
using Handler = void (*)(Event, std::uint8_t);
// open/close与运行期不重叠；handler由UI模块提供，其余调用均在UI线程。
/** @brief 在 LVGL 已初始化后创建页面；字体路径须持续有效，handler 必须非空。 */
bool open(const char* font_path, Handler handler);
/** @brief 在 UI 线程更新变化的控件；页面必须已成功 open，拖动期间不覆盖滑块位置。 */
void show(const UiView& view);
/** @brief 释放页面及字体；调用前需停止 UI 回调，允许尚未完成初始化时调用。 */
void close() noexcept;
}  // namespace boompi::ui::page
