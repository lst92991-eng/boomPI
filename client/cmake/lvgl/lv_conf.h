/**
 * @file lv_conf.h
 * @brief boomPI 页面构建使用的 LVGL 8.2 编译期配置。
 *
 * 配置通过 CMake 注入 LVGL 和页面目标；RGB565 格式同时约束 SDL 帧缓冲、摄像头图片
 * 及 ST7789P3 刷屏端口。这里不定义产品对话状态，也不保存用户运行期音量。
 */
#ifndef BOOMPI_LV_CONF_H
#define BOOMPI_LV_CONF_H

#include <stdlib.h>

// 像素保留主机 RGB565 排列；DisplayTouch::Flush 在发 SPI 时转换为面板所需高字节在前。
#define LV_COLOR_DEPTH 16
#define LV_COLOR_16_SWAP 0
#define LV_COLOR_SCREEN_TRANSP 0
// LVGL 使用 C 运行库堆，不另设静态堆；对象分配和释放留在拥有 LVGL 的 UI 线程。
#define LV_MEM_CUSTOM 1
#define LV_MEM_CUSTOM_INCLUDE <stdlib.h>
#define LV_MEM_CUSTOM_ALLOC malloc
#define LV_MEM_CUSTOM_FREE free
#define LV_MEM_CUSTOM_REALLOC realloc
// 宿主循环显式调用 lv_tick_inc：板端使用实际时间差，模拟器使用目标帧步长。
#define LV_TICK_CUSTOM 0
#define LV_USE_LOG 0
// 空指针/分配断言会 abort，避免带坏对象继续绘图；普通阶段错误由外围初始化检查处理。
#define LV_USE_ASSERT_NULL 1
#define LV_USE_ASSERT_MALLOC 1
#define LV_ASSERT_HANDLER abort();
// 保留内置拉丁字体；产品中文标签由 page::open 加载外部 FreeType 字体。
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_20 1
#define LV_USE_FREETYPE 1
// 最小版只保留语音与摄像头，不编入已删除配网页的二维码组件。
#define LV_USE_QRCODE 0
// lv_init一次创建进程级FreeType缓存；页面只管理字体，不能重复初始化缓存。
#define LV_FREETYPE_CACHE_FT_FACES 4
#define LV_FREETYPE_CACHE_FT_SIZES 4
#define LV_FREETYPE_CACHE_SIZE 65536

#endif
