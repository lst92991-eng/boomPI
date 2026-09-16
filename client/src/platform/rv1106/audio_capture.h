/** @file audio_capture.h
 * @brief 原始 ALSA 采集接口；资源生命周期由 voice_input 管理。
 */
#pragma once
#include <cstdint>
namespace boompi::audio_capture {
// 只管理原始四槽PCM；0成功、负值为ALSA错误码。
int open() noexcept;
// 固定20ms：正值为完整帧数，0为断流，-ECANCELED为主动停止，其余负值为错误。
/** @brief pcm 须容纳 960×4 个 S16；完整返回前不得把任何短读前缀交给算法。 */
int read(std::int16_t* pcm) noexcept;
// 中断阻塞读取，所属任务退出后再close。
int interrupt() noexcept;
/** @brief 仅在线程退出后释放句柄；允许初始化失败或重复关闭。 */
void close() noexcept;
}  // namespace boompi::audio_capture
