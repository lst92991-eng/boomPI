/// @file RV1106 声卡配置、完整 period I/O 和中断。
#pragma once

#include <alsa/asoundlib.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>

namespace boompi::platform::rv1106 {

class AlsaAudio final {
 public:
  AlsaAudio() noexcept = default;
  ~AlsaAudio() noexcept {
    Close();
  }
  AlsaAudio(const AlsaAudio&) = delete;
  AlsaAudio& operator=(const AlsaAudio&) = delete;

  bool Open(const std::string& capture_name, const std::string& playback_name) noexcept;
  bool ReadCapture20ms(std::int16_t* output, bool* discontinuity) noexcept;
  bool PreparePlayback() noexcept;
  bool WritePlayback(const std::int16_t* stereo, std::size_t frames) noexcept;
  bool DrainPlayback() noexcept;
  void DropPlayback() noexcept;
  // 这两个操作可由其他线程调用，让 read/write/drain 返回以便退出或取消。
  void InterruptCapture() noexcept;
  void InterruptPlayback() noexcept;
  bool playback_interrupted() const noexcept;
  std::uint64_t playback_xruns() const noexcept;
  std::string last_error() const;
  // 只在 Open/采集帧边界清理旧诊断；播放线程不得清除并发采集失败的原因。
  void ClearError() noexcept;
  // 必须先 join 采集和播放线程，再关闭句柄。
  void Close() noexcept;

 private:
  void SetError(const char* stage, int code) noexcept;
  snd_pcm_t* capture_pcm_{nullptr};
  snd_pcm_t* playback_pcm_{nullptr};
  std::atomic<bool> capture_interrupted_{false}, playback_interrupted_{false};
  std::atomic<std::uint64_t> playback_xruns_{0U};
  mutable std::mutex error_mutex_{};
  std::array<char, 192U> error_{};
};

}  // namespace boompi::platform::rv1106
