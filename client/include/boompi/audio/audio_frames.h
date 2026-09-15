/** @file audio_frames.h
 * @brief 音频各处理阶段交接的数据；采样格式由 audio_format.h 统一定义。
 *
 * 输入链依次为 RawCaptureFrame → CaptureChannels → CleanAudioFrame → CaptureFrame，
 * 对应 ALSA → 重采样拆通道 → Rockchip 3A → 检测。前三者由采集线程复用，最后一项
 * 复制进 AudioTasks 的有界队列，再交给 actor 内的 VoiceAudio 整理成语句。
 * 输出链为 16 kHz TTS → StereoPlaybackFrame → ALSA。
 */
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "boompi/audio/audio_format.h"

namespace boompi::audio {

inline constexpr std::size_t kVoiceFrameSamples16k = kVoiceFrameSamples;
using VoiceFrame16k = std::array<std::int16_t, kVoiceFrameSamples16k>;

/// 声卡原始20ms，按 [mic0,mic1,refL,refR] 交错。转换器不修改这份原始数据。
struct RawCaptureFrame final {
  std::array<std::int16_t, kCaptureFrameSamples * VoiceFrameContract::capture_channels> pcm{};
  /// ReadCapture20ms 收齐 period 后读取的 steady_clock 微秒值；不是 DMA 首样本硬件时间戳。
  std::uint64_t timestamp_us{0U};
  /// ALSA 已从 XRUN/挂起恢复；此帧会触发前端复位，不能拼进当前语句。
  bool discontinuity{false};
};

/// 描述同一段声音的采集时刻、原始麦电平与硬件参考，必须一起跟随3A延迟。
struct CaptureMetadata final {
  std::uint64_t timestamp_us{0U};
  float input_dbfs{-120.0F};
  /// 降采样后的 refL 是否出现超过固定绝对峰值门限的样本；不是 WebRTC VAD 结果。
  bool reference_active{false};
};

/// 四通道联合降采样后，取出3A所需的双麦和refL；每个平面都是16kHz/320样本。
struct CaptureChannels final {
  VoiceFrame16k mic_left{}, mic_right{}, reference_left{};
  CaptureMetadata metadata{};
};

/// 3A输出和与其对齐的元数据。检测模块不再自行查找上一帧的参考或电平。
struct CleanAudioFrame final {
  VoiceFrame16k pcm{};
  CaptureMetadata metadata{};
};

/// 检测后的20ms单声道帧；采集线程发布，语句整理消费。
struct CaptureFrame final {
  VoiceFrame16k pcm{};
  /// 处理帧序号；序号空洞或discontinuity均不能作为连续语句上传。
  std::uint64_t sequence{0U};
  std::uint64_t timestamp_us{0U};
  /// 原始双麦最大电平用于开口准入；3A后电平用于近讲判断，单位均为dBFS。
  float input_dbfs{-120.0F}, voice_dbfs{-120.0F};
  /// wake 为热词命中；vad_now 为逐帧人声，started/ended 仅在去抖状态切换时置位。
  bool wake{false}, vad_now{false}, vad_started{false}, vad_ended{false};
  /// actor_overrun 区分消费跟不上造成的缺帧；与硬件断点并发时优先标为硬件原因。
  bool discontinuity{false}, actor_overrun{false};
  /// near_voice已应用AEC预热/尾音抑制；reference_active与本帧PCM对齐。
  bool near_voice{false}, reference_active{false};
};

// 播放重采样保留两个48kHz period的容量，实际样本数以转换器返回值为准。
inline constexpr std::size_t kPlaybackFrameCapacity = 2U * kCaptureFrameSamples;
/// @brief 48 kHz 交错双声道输出；frames 数的是采样时刻，实际 S16 元素数为 frames×2。
struct StereoPlaybackFrame final {
  std::array<std::int16_t, kPlaybackFrameCapacity * VoiceFrameContract::playback_channels>
      pcm{};
  std::size_t frames{0U};
};

}  // namespace boompi::audio
