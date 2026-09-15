#pragma once

#include <array>
#include <cstddef>

#include "boompi/audio/audio_frames.h"

namespace boompi::speech {

enum class ListenMode { Wake, FollowUp };
enum class Decision { None, Wake, Start, Pcm, Barge, Fault };

// 只借用句首环或本次输入，不复制PCM；有效期到下一次update/listen/reset。
// 应用先处理Start/Barge，再逐帧发送frames[0..count)，最后按end发送END。
struct Result {
  Decision decision{Decision::None};
  std::array<const audio::CaptureFrame*, 32> frames{};
  std::size_t count{0};
  bool end{false};
  float playback_scale{1.0F};  // 插话候选的静音探测；用户音量另行保存。
};

// 仅主线程调用；本模块只拥有句首历史与准入计数，不打开设备、启线程或收发网络。
void listen(ListenMode mode) noexcept;
void reset() noexcept;
Result update(const audio::CaptureFrame& frame, bool speaking) noexcept;

}  // namespace boompi::speech
