#pragma once

#include <chrono>
#include <string>

#include "boompi/audio/audio_frames.h"

namespace boompi::audio_capture {

enum class ReadResult { Frame, Timeout, Failed };

// 先open采集资源，再open播放资源，最后start采集；保留两个PCM就绪后才read的顺序。
bool open();
bool start();
ReadResult read(audio::CaptureFrame* frame, std::chrono::milliseconds timeout);
// 命令在采集帧边界完成，最多等待100ms；旧PCM保留，但旧唤醒/VAD判定会清除。
bool reset_listener();
// 播放begin内部调用，先武装AEC保护，再允许扬声器出声。
bool arm_playback();
std::string error();
// 先关播放，再关采集；中断read、join后才释放声卡与算法。
void close();

}  // namespace boompi::audio_capture
