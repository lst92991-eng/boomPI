#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

namespace boompi::alsa_audio {
// Mode1四槽采集和双声道播放仍按48kHz/S16配置；两组句柄由各自任务独占。
bool open_capture(const std::string& name) noexcept;
bool open_playback(const std::string& name) noexcept;
// 短读继续收齐；XRUN丢弃部分帧并报告断点，不拼接断流前后数据。
bool read(std::int16_t* output, bool* discontinuity) noexcept;
bool prepare_playback() noexcept;
bool write(const std::int16_t* stereo, std::size_t frames) noexcept;
bool drain() noexcept;
void drop() noexcept;
// 只有interrupt允许跨线程中断阻塞I/O；close必须在所属任务join之后。
void interrupt_capture() noexcept;
void interrupt_playback() noexcept;
bool playback_interrupted() noexcept;
std::string capture_error();
std::string playback_error();
void clear_playback_error() noexcept;
void close_capture() noexcept;
void close_playback() noexcept;
}  // namespace boompi::alsa_audio
