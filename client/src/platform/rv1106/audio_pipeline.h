/**
 * @file audio_pipeline.h
 * @brief AudioTasks 内部使用的 RV1106 同步音频处理链。
 *
 * 输入是 ALSA 四通道 period，输出是可供 VoiceAudio 消费的清洁 PCM 与检测标志；
 * 下行接口将 16 kHz TTS 转成声卡双声道。具体线程与队列生命周期由 AudioTasks 管理。
 */
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>

#include "alsa_audio.h"
#include "audio_converter.h"
#include "boompi/audio/audio_frames.h"
#include "boompi/platform/rv1106/rockchip_voice_dsp.h"
#include "speech_detector.h"

namespace boompi::platform::rv1106 {

using audio::CaptureFrame;
using audio::RawCaptureFrame;

/// @brief RV1106 同步音频后端，由 AudioTasks 的两条实时线程直接调用。
///
/// AudioTasks 的 capture 线程独占 capture/3A/Snowboy/VAD。播放激活期间，播放线程独占 TTS
/// 重采样和 playback PCM；AEC reference 由 capture PCM 的 Mode1 硬件回采提供。
/// 类内不创建线程或队列，调用顺序和线程所有权由 AudioTasks 保证。
class AudioPipeline final {
 public:
  AudioPipeline() noexcept = default;
  ~AudioPipeline() noexcept;
  AudioPipeline(const AudioPipeline&) = delete;
  AudioPipeline& operator=(const AudioPipeline&) = delete;

  /// @brief 一次性打开 ALSA、Mode1、重采样器、3A、Snowboy 和 VAD。
  /// @return 任一阶段失败会释放已经创建的资源，失败阶段由 `LastError` 返回。
  bool Open() noexcept;
  /// @brief 从 ALSA 完整读取 960 个四通道 frame，即 48 kHz 下的 20 ms。
  /// @param raw 输出原始 PCM、收齐 period 后的单调时钟观测值及 ALSA 恢复后的断点标记。
  /// 部分读取会在本函数内续读；发生 recover 后丢弃不完整 period 并从头收集。
  /// 成功返回 true；中断/失败返回 false，调用方不处理未完成的输出。
  bool ReadCapture20ms(RawCaptureFrame* raw) noexcept;
  /// @brief 处理刚读到的 period，依次执行联合重采样、拆平面、3A、Snowboy 与 VAD。
  /// @param raw 原始采集结果；有断点时只复位前端并交付断点标记，不继续处理该帧。
  /// @param frame 接收与 DSP 固定延迟对齐的 16 kHz/mono/20 ms 结果和判定元数据。
  bool ProcessCapture20ms(const RawCaptureFrame& raw, CaptureFrame* frame) noexcept;
  /// @brief 清除 Snowboy/VAD 判定历史，保留 ALSA、重采样器和 3A 连续状态。
  bool ResetListener() noexcept;

  /// @brief capture 在帧边界武装 AEC 门控，完成后才允许播放线程准备和渲染。
  /// 预热计数等首个非静音 Mode1 reference 才开始，网络首播蓄水不会被计入。
  bool ArmPlayback() noexcept;
  /// @brief 播放线程在首帧前准备 PCM 和播放重采样器，全程不修改 capture 状态。
  bool PreparePlayback() noexcept;
  /// @brief 将16kHz mono TTS的有效样本直接升采样、限幅并写入48kHz stereo ALSA。
  /// @param samples 有效 sample 数，允许 EOS 前最后一帧短于 320。
  /// @param gain 用户音量乘以本轮探测衰减；必须是有限非负值。
  /// PCM 仅在调用期间借用；写入阻塞或失败由播放线程统一决定 drain/drop。
  bool Render20ms(const std::int16_t* pcm16, std::size_t samples, float gain) noexcept;
  /// @brief 自然播放结束：取完重采样滤波尾音，再等待ALSA播完；下一轮首包前prepare。
  bool DrainPlayback() noexcept;
  /// @brief 主动打断：丢弃 ALSA 剩余数据，发布播放结束事实供采集侧更新门控。
  /// 硬件参考/房间尾音是否消退仍需读取后续采集，发布结束不等于测量到没有回声。
  void DropPlayback() noexcept;
  /// 唤醒可能阻塞在 snd_pcm_readi 的采集线程，仅用于 AudioTasks::Stop。
  void InterruptCapture() noexcept;
  /// actor 解除 write/drain 阻塞并发布取消；播放线程随后执行 DropPlayback 收尾。
  void InterruptPlayback() noexcept;

  /// @brief 返回最近板级错误。字符串由独立锁保护，可供 application 并发读取。
  std::string LastError() const;
  /// @brief 幂等释放所有板级资源；AudioTasks 必须先 join 两条调用线程。
  void Close() noexcept;

 private:
  bool Fail(const char* text) noexcept;
  void ClearError() noexcept;
  bool ResetFrontEnd() noexcept;
  PlaybackState ReadPlaybackState() noexcept;
  void PublishPlaybackEnd() noexcept;

  AlsaAudio pcm;
  AudioConverter converter;
  RockchipVoiceDsp dsp;
  SpeechDetector detector;
  // 中间结果只由所属线程使用，启动时分配，逐帧复用。
  audio::CaptureChannels channels;
  audio::CleanAudioFrame clean;
  audio::StereoPlaybackFrame stereo;
  float playback_gain{1.0F};  // 最后一包的增益也用于重采样器中的滤波尾音。
  bool open{false};
  // 播放线程发布、采集线程观察；这些是渲染事实，不能替代同步回采的 refL 信号。
  std::atomic<bool> playback_render_started{false};
  std::atomic<bool> playback_output_audible{false};
  std::atomic<bool> playback_ended{false};
  mutable std::mutex error_mutex;
  std::array<char, 192U> error{};
};

}  // namespace boompi::platform::rv1106
