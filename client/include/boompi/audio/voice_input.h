/** @file voice_input.h
 * @brief 应用与实时输入任务的交接点；算法调用顺序在 voice_input.cpp 的 capture_task。
 */
#pragma once
#include <string>

#include "boompi/audio/audio_frames.h"
namespace boompi::voice_input {
enum class ReadResult { Frame, Timeout, Failed };
// 应用独占open/start/read/close生命周期，close前先停止调用read。
// 配置输入资源；两路PCM均已open之后才start。独立线程顺序执行3A、唤醒和VAD，持续产出处理帧。
/** @brief 顺序配置 ALSA、转换、3A、唤醒、VAD；失败回收已取得的输入资源。 */
bool open();
/** @brief 在播放 PCM 也已配置后启动采集线程；失败关闭输入资源。 */
bool start();
// 每次最多等20ms，让主流程能继续处理回复与用户操作。
/** @brief Frame 才写入有效交接数据；Timeout 继续主循环，Failed 应退出并读取 error。 */
ReadResult read(audio::CaptureFrame& frame);
// 外部VAD句尾只通知Snowboy复位；不清PCM、不等待回执。
void end_utterance() noexcept;
/** @brief 在锁内复制错误原因，返回的字符串由调用方持有。 */
std::string error();
/** @brief 中断 ALSA 读取，等待采集线程退出，再释放算法及 PCM；允许部分初始化后调用。 */
void close();
}  // namespace boompi::voice_input
