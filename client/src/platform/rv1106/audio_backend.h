#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "boompi/audio/audio_engine.h"

namespace boompi::platform::rv1106 {

using audio::AudioEngineConfig; using audio::CaptureFrame;

/// @brief RV1106 同步音频后端，不是对 application 暴露的第二套音频接口。
///
/// AudioEngine 的 capture 线程独占 capture/3A/Snowboy/VAD。播放激活期间，播放线程独占 TTS
/// 重采样和 playback PCM；AEC reference 由 capture PCM 的 Mode1 硬件回采提供。
/// 类内不创建线程或队列。
class AudioBackend final {
 public:
  AudioBackend() noexcept = default; ~AudioBackend() noexcept;
  AudioBackend(const AudioBackend&) = delete;
  AudioBackend& operator=(const AudioBackend&) = delete;

  /// 一次性打开所有板级资源；不创建线程，失败原因由 `last_error` 返回。
  bool Open(const AudioEngineConfig& config) noexcept;
  /// capture actor 先完整读取 960 个四通道 frame，再调用 ProcessCapture20ms。
  bool ReadCapture20ms(bool* discontinuity) noexcept;
  /// 执行四通道联合 48->16 kHz、拆分、唯一 Rockchip 3A、Snowboy 与 VAD。
  bool ProcessCapture20ms(bool discontinuity, CaptureFrame* frame) noexcept;
  bool ResetListener() noexcept;

  /// 仅由 actor 在播放未激活、持有 AudioEngine mutex 时复位播放重采样器并武装 AEC 预热。
  /// 预热计数由 capture 侧检测到首个非静音 Mode1 reference 后才开始。
  bool PreparePlayback() noexcept;
  bool Render20ms(const std::int16_t* pcm24, std::size_t samples,
                  float gain) noexcept;
  bool DrainPlayback() noexcept;
  void DropPlayback() noexcept;
  void InterruptPlayback() noexcept;

  /// last_error 由独立锁保护；Close 必须在播放线程 join 后调用。
  std::string last_error() const;
  void Close() noexcept;

 private:
  struct Impl;
  Impl* impl_{nullptr};
};

}  // namespace boompi::platform::rv1106
