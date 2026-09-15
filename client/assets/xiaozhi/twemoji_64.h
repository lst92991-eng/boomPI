/**
 * @file twemoji_64.h
 * @brief 小智状态页面使用的五张 64x64 静态 LVGL 表情资源声明。
 *
 * 对应 .c 保存带 alpha 的已转换像素及描述符，LvglScreen 的 kVoiceViews 按显示状态
 * 选择资源，再由 lv_img_set_src 使用；运行期不解码 PNG，也不分配动画帧。
 * 资源来源和许可证见同目录 NOTICE.md；extern C 使 C 资源定义与 C++ 页面符号一致。
 */
#pragma once

#include <lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 回答中使用的笑脸；描述符和像素均在程序整个生命周期内有效。 */
extern const lv_img_dsc_t emoji_1f606_64;
/** @brief 离线与错误共用的低落表情。 */
extern const lv_img_dsc_t emoji_1f614_64;
/** @brief 聆听状态使用的表情。 */
extern const lv_img_dsc_t emoji_1f62f_64;
/** @brief 空闲与回答完成状态使用的微笑表情。 */
extern const lv_img_dsc_t emoji_1f642_64;
/** @brief 等待回答时使用的思考表情。 */
extern const lv_img_dsc_t emoji_1f914_64;

#ifdef __cplusplus
}
#endif
