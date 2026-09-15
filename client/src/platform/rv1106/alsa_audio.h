/**
 * @file alsa_audio.h
 * @brief AudioPipeline 使用的 RV1106 声卡配置、完整 period I/O 和跨线程中断接口。
 *
 * capture 固定为 48 kHz/S16_LE/4 通道，playback 为 48 kHz/S16_LE/2 通道。
 * 一个 ALSA frame 指同一时刻的所有通道，不能把 frames 与 S16 元素个数混用。
 */
#pragma once

#include <alsa/asoundlib.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>

namespace boompi::platform::rv1106 {

/**
 * @brief 分别持有采集和播放 PCM 句柄，并把阶段错误保存为可并发读取的诊断文本。
 *
 * Open 在工作线程启动前调用；运行期 capture 线程独占读取，playback 线程独占
 * prepare/write/drain/drop。Interrupt 系列用于 actor 解除阻塞，Close 必须等线程退出。
 */
class AlsaAudio final {
 public:
  AlsaAudio() noexcept = default;
  ~AlsaAudio() noexcept {
    Close();
  }
  AlsaAudio(const AlsaAudio&) = delete;
  AlsaAudio& operator=(const AlsaAudio&) = delete;

  /// @brief 先选择 Codec Mode1 再重新打开并严格配置两个 PCM，失败自动关闭已开句柄。
  /// 调用方须保证尚未打开；成功后才可启动读写线程，诊断保留失败阶段及 ALSA 原因。
  bool Open(const std::string& capture_name, const std::string& playback_name) noexcept;
  /**
   * @brief 阻塞收齐 960 个四通道采样时刻，输出缓冲至少容纳 3840 个 S16 元素。
   * @param discontinuity 成功恢复 XRUN/挂起时置 true；之前的不完整 period 会从头收集。
   * @return 完整 period 返回 true；中断或不可恢复错误返回 false，不能使用半帧。
   */
  bool ReadCapture20ms(std::int16_t* output, bool* discontinuity) noexcept;
  /// 播放线程开始新流前 prepare，成功后才清除上次 interrupted 标志；失败返回 false。
  bool PreparePlayback() noexcept;
  /// 将 frames 个 48 kHz 双声道采样时刻完整写入；部分写继续，取消/失败返回 false。
  /// 句柄和 stereo 缓冲由 AudioPipeline 保证有效；发生 XRUN 只续写未被 ALSA 接收的后缀。
  bool WritePlayback(const std::int16_t* stereo, std::size_t frames) noexcept;
  /// 自然END阻塞等待内核尾播；下一轮首包前再prepare，失败返回false。
  bool DrainPlayback() noexcept;
  /// 播放线程收尾时丢弃内核剩余样本；错误写诊断，不清interrupted标志。
  void DropPlayback() noexcept;
  // 这两个操作可由其他线程调用，让 read/write/drain 返回以便退出或取消。
  void InterruptCapture() noexcept;
  void InterruptPlayback() noexcept;
  /// 上次 prepare 后是否收到取消；采集侧用此区分自然结束与主动插话。
  bool WasPlaybackInterrupted() const noexcept;
  /// 锁内复制最近错误；成功的正常 I/O 不清掉既有错误文本。
  std::string LastError() const;
  // 只在 Open/采集帧边界清理旧诊断；播放线程不得清除并发采集失败的原因。
  void ClearError() noexcept;
  // 必须先 join 采集和播放线程，再关闭句柄；可重复调用，保持板卡 Mode1 不回退。
  void Close() noexcept;

 private:
  bool Fail(const char* stage, int code) noexcept;
  snd_pcm_t* capture_pcm_{nullptr};
  snd_pcm_t* playback_pcm_{nullptr};
  std::atomic<bool> capture_interrupted_{false}, playback_interrupted_{false};
  mutable std::mutex error_mutex_{};
  std::array<char, 192U> error_{};
};

}  // namespace boompi::platform::rv1106
