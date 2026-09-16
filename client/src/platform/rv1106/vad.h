/** @file vad.h
 * @brief 逐帧人声判断，输入必须是 16k/S16/mono 的 320 点处理结果。
 */
#pragma once
#include "boompi/audio/audio_frames.h"
namespace boompi::vad {
/** @brief 创建 VAD 并检查帧格式；失败释放句柄，不能重复打开同一实例。 */
bool open() noexcept;
/** @brief 断点后恢复初始 VAD 状态和模式 3，避免沿用断点前的判断历史。 */
bool reset() noexcept;
// -1为错误，0为无人声，1为人声；不会把错误折算为静音。
int process(const audio::VoiceFrame16k& pcm) noexcept;
/** @brief 采集线程停止后释放句柄，允许重复调用。 */
void close() noexcept;
}  // namespace boompi::vad
