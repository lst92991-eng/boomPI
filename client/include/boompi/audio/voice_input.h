#pragma once
#include <chrono>
#include <string>

#include "boompi/audio/audio_frames.h"
namespace boompi::voice_input {
enum class ReadResult { Frame, Timeout, Failed };
// 配置输入资源；两路PCM均已open之后才start。独立线程持续产出3A结果。
bool open();
bool start();
ReadResult read(audio::CaptureFrame* frame, std::chrono::milliseconds timeout);
std::string error();
void close();
}  // namespace boompi::voice_input
