/** @file audio_convert.h
 * @brief 隔离 libswresample；采集、播放的转换状态分别属于各自工作线程。
 */
#pragma once
#include "boompi/audio/audio_frames.h"

namespace audio_convert
{
// 每个方向由所属任务独占：open成功后处理，任务退出后close；不接受未初始化调用。
/** @brief 建立 48k 四槽到 16k 双麦/单参考的转换，失败释放本方向资源。 */
bool open_capture();
/** @brief 建立 16k mono 到 48k stereo 的转换，明确 L=R=mono，失败释放本方向资源。 */
bool open_playback();
/** @brief 断点后清旧滤波历史并用静音预填，恢复固定业务帧输出；仅 open 成功后调用。 */
bool reset_capture();
/** @brief 新回答前丢弃上一轮的滤波历史与欠输出计数；仅 open 成功后调用。 */
bool reset_playback();
// 保留原始四槽数据，联合降采样后直接交付交错的双麦/单参考。
bool capture(const audio::RawCaptureFrame &raw, audio::CaptureChannels &output);
// nullptr/0表示取出有效尾音；静音推进内部滤波，输出长度由真实输入的采样时刻限定。
bool playback(const std::int16_t *pcm, std::size_t samples, audio::StereoPlaybackFrame &output);
/** @brief 输入线程退出后释放采集转换器，允许重复调用。 */
void close_capture();
/** @brief 播放线程退出后释放播放转换器及尾音计数，允许重复调用。 */
void close_playback();
}  // namespace audio_convert
