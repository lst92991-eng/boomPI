/** @file audio_frames.h
 * @brief 音频各处理阶段交接的数据；采样格式由 audio_format.h 统一定义。
 *
 * 输入线程依次处理RawCaptureFrame → CaptureChannels → CaptureFrame。
 * 3A直接写交付帧，应用再调用wake/VAD/speech；没有clean→frame的PCM中转。
 * 输出链为 16 kHz TTS → StereoPlaybackFrame → ALSA。
 */
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "boompi/audio/audio_format.h"
#include "boompi/audio/playback.h"

namespace boompi::audio {

using VoiceFrame16k = std::array<std::int16_t, kVoiceFrameSamples>;

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

/// 双麦和refL联合降采样，每个平面16kHz/320样本；refR不参与软件转换。
struct CaptureChannels final {
  VoiceFrame16k mic_left{}, mic_right{}, reference_left{};
  CaptureMetadata metadata{};
};

/// 3A后的20ms单声道帧；应用填入检测/语句判断，不回写采集线程。
struct CaptureFrame final {
  VoiceFrame16k pcm{};
  // 与生产该帧时的播放事实一起排队，应用延迟消费时不读取未来的结束事件。
  playback::Observation output{};
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
