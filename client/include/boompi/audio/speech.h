/** @file speech.h
 * @brief 主线程中的语句边界与插话策略；只决定应交付哪些帧，不执行网络或播放操作。
 */
#pragma once
#include "boompi/audio/audio_frames.h"

namespace boompi::speech {
inline constexpr std::size_t kPreRollFrames = 500 / audio::kFrameMs;
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
// 仅在应用Listening/Speaking时调用；开始监听时reset，END后停止update。
// PCM借用到下一次update/reset。调用方先处理断点，依次START、frames、END。
/** @brief 开始新的监听窗口时清前滚与确认计数；正常尾播转追问不调用，以保留已开口部分。 */
void reset() noexcept;
// speaking指当前有声回复；自然尾播仍在本模块短暂保留回声上下文。
/** @brief 每个连续输入帧调用一次；仅 start 后才上传，end 的帧仍须发送后再 END。
 * @param speaking 当前正在有声回复；静音音量下传 false，不要求等待不存在的回声参考。
 */
Result update(const audio::CaptureFrame& frame, bool speaking = false) noexcept;
}  // namespace boompi::speech
