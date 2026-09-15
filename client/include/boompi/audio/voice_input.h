#pragma once
#include <string>

#include "boompi/audio/audio_frames.h"
namespace boompi::voice_input {
enum class ReadResult { Frame, Timeout, Failed };
// 应用独占open/start/read/close生命周期，close前先停止调用read。
// 配置输入资源；两路PCM均已open之后才start。独立线程顺序执行3A、唤醒和VAD，持续产出处理帧。
bool open();
bool start();
// 每次最多等20ms，让主流程能继续处理回复与用户操作。
ReadResult read(audio::CaptureFrame& frame);
// 外部VAD句尾只通知Snowboy复位；不清PCM、不等待回执。
void end_utterance() noexcept;
std::string error();
void close();
}  // namespace boompi::voice_input
