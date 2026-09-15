/** @file audio_converter.h
 * @brief 四通道采集格式转换与 TTS 播放格式转换。
 *
 * AudioPipeline 输入链先把 RawCaptureFrame 变成双麦/refL 平面；输出链先升采样，
 * 直接输出双声道，再按同帧峰值限幅、应用音量，供 AlsaAudio 直接写入。
 */
#pragma once

#include "boompi/audio/audio_frames.h"

struct SwrContext;

namespace boompi::platform::rv1106 {

/// 去除直流偏置后计算 RMS 电平；转换器和检测器共用这项无状态计算，结果单位 dBFS。
/// 零交流能量返回 -120；满幅基准取 32768，用于原始麦准入与清洁 PCM 的不同门限。
float AcRmsDbfs(const audio::VoiceFrame16k& samples) noexcept;

/**
 * @brief 持有两个方向各自的重采样历史和预分配缓冲。
 *
 * 采集线程独占 ConvertCapture/ResetCapture，播放线程独占播放转换/ResetPlayback。
 * Open/Close 只在两条线程尚未启动或已经退出时调用，不在每帧分配内存。
 */
class AudioConverter final {
 public:
  AudioConverter() noexcept = default;
  ~AudioConverter() noexcept;
  AudioConverter(const AudioConverter&) = delete;
  AudioConverter& operator=(const AudioConverter&) = delete;

  /// 创建重采样器；首帧前由调用方分别 ResetCapture/ResetPlayback 清除历史。
  /// 两个 polarity 仅接受 +1/-1；任一分配/初始化失败均释放已建资源并返回 false。
  bool Open(std::int8_t left_polarity, std::int8_t right_polarity) noexcept;
  /// 输入断点后清历史并预送静音，丢弃预热输出；正常连续帧不能逐帧 reset。
  bool ResetCapture() noexcept;
  /// 新 TTS 开始前清除上一轮滤波尾巴；只能由播放线程在首帧渲染前调用。
  bool ResetPlayback() noexcept;

  /// 保留 raw；四通道共同降采样后，交出双麦、refL 和本帧采集元数据。
  /// 必须一次生成 320 个采样时刻才成功；输出长度异常返回 false，后端不补零掩盖失配。
  bool ConvertCapture(const audio::RawCaptureFrame& raw,
                      audio::CaptureChannels* output) noexcept;
  /// 直接转换有效单声道输入为双声道；不为满足20ms而补零，输出长度由滤波器决定。
  /// pcm=nullptr、samples=0 表示 EOS，内部静音推进滤波器但只交付原输入有效时长。
  /// 成功且frames=0表示取完；即使整个回复只有1样本也能输出，不能直接依赖null flush。
  bool UpsamplePlayback(const std::int16_t* pcm, std::size_t samples,
                        audio::StereoPlaybackFrame* output) noexcept;
  /// 无状态扫描有效样本范围，返回绝对峰值；提升到 long 后可表示 abs(-32768)。
  static long Peak(const audio::StereoPlaybackFrame& frame) noexcept;
  /// peak 必须来自同一帧；限幅先于增益，非法增益按静音处理。
  static void ApplyVolume(audio::StereoPlaybackFrame* frame, float gain, long peak) noexcept;
  /// 幂等释放两个 SwrContext 并清空工作缓冲，必须避开正在转换的音频线程。
  void Close() noexcept;

 private:
  // 两个方向的滤波历史完全分开，因此 capture/playback 可并行处理各自的缓冲。
  SwrContext* capture_swr_{nullptr};
  SwrContext* playback_swr_{nullptr};
  std::size_t playback_pending_frames_{0U};
  std::int8_t left_polarity_{1}, right_polarity_{1};
  std::array<std::int16_t,
             audio::kCaptureFrameSamples * audio::VoiceFrameContract::capture_channels>
      corrected48_{};
  std::array<std::int16_t,
             audio::kVoiceFrameSamples * audio::VoiceFrameContract::capture_channels>
      interleaved16_{};
};

}  // namespace boompi::platform::rv1106
