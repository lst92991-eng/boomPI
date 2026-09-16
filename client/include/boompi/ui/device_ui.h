/** @file device_ui.h
 * @brief 应用和 UI 线程交换显示快照、触摸动作；不把 LVGL 类型暴露给应用。
 */
#pragma once
#include "boompi/ui/ui_view.h"

namespace boompi::ui {
// 应用串行管理生命周期；运行期仅UI线程调用LVGL。
/** @brief 读取已保存的音量；文件缺失或值无效则采用限制到 0..100 的 fallback。 */
std::uint8_t load_volume(std::uint8_t fallback = 60) noexcept;
/** @brief 硬件 → LVGL 端口 → 字体/页面 → UI 线程；失败清资源，应用可继续提供语音。 */
bool open();
/** @brief 短锁内复制最新画面并通知 UI；不绘制、不等 SPI，旧的未显示快照可被覆盖。 */
void show(const UiView& view) noexcept;
/** @brief 非阻塞读取合并后的动作；开始/停止优先于音量，拖动只保留最新百分比。 */
bool poll_action(UiAction& action) noexcept;
/** @brief 先停止并 join UI，再释放页面、输入/显示端口和硬件；初始化失败时也可调用。 */
void close() noexcept;
}  // namespace boompi::ui
