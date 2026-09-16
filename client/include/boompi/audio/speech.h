/** @file speech.h
 * @brief 主线程中的语句边界与插话策略；只决定应交付哪些帧，不执行网络或播放操作。
 */
#pragma once
#include "boompi/audio/audio_frames.h"

namespace boompi::speech {
constexpr std::size_t kPreRollFrames = 500 / audio::kFrameMs;
/** @brief 本次处理产生的动作。按 start → frames[0..count) → end 的顺序消费。
 * 帧指针借用 history 或本次输入：必须在下一次 update/reset 或输入失效前消费。
 * 这里不再复制一遍 PCM；网络 send 在调用期间完成编码。
 */
struct Result {
  bool start{false};
  std::array<const audio::VoiceFrame16k*, kPreRollFrames> frames{};
  std::size_t count{0};
  bool end{false};
  bool hold_playback{false};  // 候选只请求短暂停播，确认后才start。
};
/** @brief 开始新的监听窗口时清前滚与确认计数；正常尾播转追问不调用，以保留已开口部分。 */
void reset() noexcept;

/**
 * @brief 逐帧判断语句开始、句尾及插话。
 *
 * 仅在 Listening/Speaking 时调用，END 后停止。
 * 自然尾播进入追问时保留前滚和回声上下文。
 *
 * @param frame 当前连续输入帧。
 * @param speaking 本机是否正在有声播放；静音音量传 false。
 */
Result update(const audio::CaptureFrame& frame, bool speaking = false) noexcept;
}  // namespace boompi::speech
