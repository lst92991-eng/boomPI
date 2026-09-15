#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "boompi/audio/audio_frames.h"

// 只替换声卡设备调用。采集任务、转换、3A分块、检测、播放线程和EOS均执行产品代码。
namespace boompi::test::audio_hardware {
enum class PlaybackBlock { None, Prepare, Write, Drain };
void reset();
void push_capture(const audio::RawCaptureFrame& frame, bool gap = false);
void block_playback(PlaybackBlock stage);
void fail_playback_preparation(bool fail = true);
void fail_capture_open(bool fail);
bool wait_for_capture_reads(std::size_t count, std::chrono::milliseconds timeout);
bool wait_for_writes(std::size_t count, std::chrono::milliseconds timeout);
bool wait_for_playback_blocked(std::chrono::milliseconds timeout);
std::size_t capture_reads();
std::size_t capture_interrupts();
std::size_t write_count();
std::size_t drain_count();
bool owner_order_valid();
// 样本已经经过真实16→48k、左右复制、音量和EOS处理；每个采样时刻两个元素。
std::vector<std::int16_t> written_samples();
std::vector<std::string> operations();
}  // namespace boompi::test::audio_hardware
