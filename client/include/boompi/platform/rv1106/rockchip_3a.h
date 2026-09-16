/** @file rockchip_3a.h
 * @brief 单个 Rockchip 声学处理器的生命周期，不暴露厂商参数树。
 */
#pragma once
#include "boompi/audio/audio_frames.h"

namespace rockchip_3a
{
// 本模块直接拥有RKAUDIOParam树与vendor句柄；只在采集线程或其启动/退出边界调用。
/** @brief 建立参数树并初始化厂商句柄，失败释放已取得的树和句柄。 */
bool open();
// 当前SDK适配契约为256点块；320点输入/输出通过固定块余数对齐。
/** @brief 输入交错 mic0/mic1/refL，输出 mono；失败关闭处理器，输出不可继续使用。
 * 必须先 open；块余数由本模块持有，不借用输入输出数组到下次调用。
 */
bool process(const audio::CaptureChannels &input, audio::VoiceFrame16k &pcm);
/** @brief 先销毁算法句柄，再释放参数树；断点重建和最终退出共用。 */
void close();
}  // namespace rockchip_3a
