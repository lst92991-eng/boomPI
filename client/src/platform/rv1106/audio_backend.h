/// @file AudioEngine 内部使用的 RV1106 同步音频后端接口。
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "boompi/audio/audio_engine.h"

namespace boompi::platform::rv1106 {

using audio::AudioEngineConfig; using audio::CaptureFrame;

/// @brief RV1106 同步音频后端，由 AudioEngine 的两条实时线程直接调用。
///
/// AudioEngine 的 capture 线程独占 capture/3A/Snowboy/VAD。播放激活期间，播放线程独占 TTS
/// 重采样和 playback PCM；AEC reference 由 capture PCM 的 Mode1 硬件回采提供。
/// 类内不创建线程或队列，调用顺序和线程所有权由 AudioEngine 保证。
class AudioBackend final {
 public:
  AudioBackend() noexcept = default; ~AudioBackend() noexcept;
  AudioBackend(const AudioBackend&) = delete;
  AudioBackend& operator=(const AudioBackend&) = delete;

  /// @brief 一次性打开 ALSA、Mode1、重采样器、3A、Snowboy 和 VAD。
  /// @return 任一阶段失败会释放已经创建的资源，失败阶段由 `last_error` 返回。
  bool Open(const AudioEngineConfig& config) noexcept;
  /// @brief 从 ALSA 完整读取 960 个四通道 frame，即 48 kHz 下的 20 ms。
  /// @param discontinuity 输出 ALSA XRUN/挂起恢复是否打断了媒体时间线。
  /// 部分读取会在本函数内续读；发生 recover 后丢弃不完整 period 并从头收集。
  bool ReadCapture20ms(bool* discontinuity) noexcept;
  /// @brief 处理刚读到的 period，依次执行联合重采样、拆平面、3A、Snowboy 与 VAD。
  /// @param discontinuity 为 true 时只复位有历史状态的前端，并返回带断点标记的空帧。
  /// @param frame 接收与 DSP 固定延迟对齐的 16 kHz/mono/20 ms 结果和判定元数据。
  bool ProcessCapture20ms(bool discontinuity, CaptureFrame* frame) noexcept;
  /// @brief 清除 Snowboy/VAD 判定历史，保留 ALSA、重采样器和 3A 连续状态。
  bool ResetListener() noexcept;

  /// @brief 由 capture 线程在帧边界复位播放重采样器并武装 AEC 预热。
  /// application 通过 AudioEngine 的同步命令等待它完成；预热计数在 capture 侧检测到
  /// 首个非静音 Mode1 reference 后才开始，因此网络首播蓄水时间不会被误算进预热。
  bool PreparePlayback() noexcept;
  /// @brief 将一帧 24 kHz mono TTS 补齐、升采样、限幅并写入 48 kHz stereo ALSA。
  /// @param samples 有效 sample 数，允许 EOS 前最后一帧短于 480。
  bool Render20ms(const std::int16_t* pcm24, std::size_t samples,
                  float gain) noexcept;
  /// @brief 自然播放结束：等待 ALSA 播完已提交数据，再恢复到 prepared 状态。
  bool DrainPlayback() noexcept;
  /// @brief 主动打断：立即丢弃 ALSA 中尚未播放的数据并记录参考尾音已经结束。
  void DropPlayback() noexcept;
  /// 唤醒可能阻塞在 snd_pcm_readi 的采集线程，仅用于 AudioEngine::Close。
  void InterruptCapture() noexcept;
  void InterruptPlayback() noexcept;

  /// @brief 返回本次 Open 生命周期内成功恢复的 playback XRUN 数量。
  std::uint64_t playback_xruns() const noexcept;
  /// @brief 返回最近板级错误。字符串由独立锁保护，可供 application 并发读取。
  std::string last_error() const;
  /// @brief 幂等释放所有板级资源；AudioEngine 必须先 join 两条调用线程。
  void Close() noexcept;

 private:
  struct Impl;
  Impl* impl_{nullptr};
};

}  // namespace boompi::platform::rv1106
