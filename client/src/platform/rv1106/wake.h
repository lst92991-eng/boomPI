/** @file wake.h
 * @brief 唤醒算法接口只传固定 PCM 和基础类型，不让 Snowboy 的旧 ABI 类型跨文件。
 */
#pragma once
#include "boompi/audio/audio_frames.h"
namespace boompi::wake {
/** @brief 加载既定资源/模型、设置灵敏度并核对格式；失败释放 detector。 */
bool open() noexcept;
// open成功后由输入线程调用：-1错误、0未命中、1唤醒。
int detect(const audio::VoiceFrame16k& pcm) noexcept;
/** @brief 外部语句结束后由采集线程复位检测器；失败交给输入任务报告。 */
bool reset() noexcept;
/** @brief 采集已停止后释放模型和检测器；初始化失败时也可调用。 */
void close() noexcept;
}  // namespace boompi::wake
