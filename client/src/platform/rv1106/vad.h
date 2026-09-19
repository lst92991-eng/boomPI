/** @file vad.h
 * @brief 逐帧人声判断，输入必须是 16k/S16/mono 的 320 点处理结果。
 */
#pragma once
#include "boompi/audio/audio_frames.h"
namespace vad
{
/** @brief 创建VAD并检查帧格式；调用前保持关闭状态，失败时释放句柄。 */
bool open();
/** @brief 断点后恢复初始 VAD 状态和模式 3，使后续判断从新的连续音频开始。 */
bool reset();
// 区分错误、静音、当前帧命中及语音延续，避免把延续帧当成新的插话证据。
audio::VoiceActivity process(const audio::VoiceFrame16k &pcm);
/** @brief 采集线程停止后释放句柄，允许重复调用。 */
void close();
}  // namespace vad
