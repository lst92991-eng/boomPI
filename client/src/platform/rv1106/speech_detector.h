/**
 * @file speech_detector.h
 * @brief 从 3A 后的单声道帧提取唤醒、人声边沿和近讲候选。
 *
 * 输入固定为 16 kHz、S16、单声道的 20 ms 帧。采集线程依次调用 Detect 和
 * GateNearVoice，VoiceAudio 再结合电平与持续时间确认近讲；本模块不控制播放停止。
 * 在调用链中，上游是 AudioPipeline 的联合重采样和 RockchipVoiceDsp，下游是
 * AudioTasks 的采集队列。读 Detect 看语句边沿，读 GateNearVoice 看播放期间的保护。
 */
#pragma once

#include <cstdint>

extern "C" {
#include <webrtc_vad.h>
}

#include "boompi/audio/audio_frames.h"
#include "snowboy_legacy_bridge.h"

namespace boompi::platform::rv1106 {

/// 播放结束原因：自然结束需要尾音抑制，主动打断则保留当前人声的 VAD 历史。
enum class PlaybackEnd : std::uint8_t { None, Natural, Interrupted };

/**
 * @brief 本帧 Detect 完成后，由后端读取并传入的播放状态。
 *
 * 各字段分别读取，不保证是同一时刻的整体快照；end 由调用者消费一次，避免重复
 * 启动尾音抑制。渲染标志在 ALSA 写入前发布，实际参考是否出现以采集帧为准。
 */
struct PlaybackState final {
  /// 本轮已经处理过非零播放 PCM；不代表扬声器已经发声。
  bool render_started{false};
  /// 最近一帧非零播放 PCM 的 gain > 0；不是对扬声器声压的测量。
  bool output_audible{false};
  PlaybackEnd end{PlaybackEnd::None};
};

/**
 * @brief 持有 Snowboy、WebRTC VAD 及跨帧检测历史。
 *
 * 运行期由采集线程独占，播放线程通过后端传递状态，不直接调用本对象。
 * Open 在采集线程启动前调用，Close 和析构须避开采集调用；对象内部不加锁。
 */
class SpeechDetector final {
 public:
  SpeechDetector() noexcept = default;
  ~SpeechDetector() noexcept;
  SpeechDetector(const SpeechDetector&) = delete;
  SpeechDetector& operator=(const SpeechDetector&) = delete;

  /**
   * @brief 加载唤醒资源并检查 VAD 的采样率和帧长支持。
   * 模型和必要声学常量来自内部板级预置，不作为调用参数。
   * @return 成功返回 true；初始化中途失败会释放已申请的库资源，原因见 LastError。
   */
  bool Open() noexcept;
  /// 复位唤醒和 VAD 历史，保留播放门控；失败返回 false，后端须终止本次处理。
  bool ResetListener() noexcept;
  /// 采集线程在允许新一轮播放前启用保护；等真实参考出现后才开始 AEC 预热计时。
  void ArmPlayback() noexcept;
  /// 采集断点后重新启用活动播放的保护；后端还须依次复位重采样、3A 和检测历史。
  void OnDiscontinuity() noexcept;
  /**
   * @brief 对连续的 20 ms 帧执行唤醒、VAD 和语句起止边沿检测。
   * @param clean PCM 与时间戳、原始电平和参考标志已由 3A 对齐，此处不再延迟。
   * @param frame 非空输出，通过就绪检查后先清空再填充；sequence 由调用者发布时分配。
   * @return 成功返回 true；失败时输出可能只填了一部分，不得发布为有效采集帧。
   */
  bool Detect(const audio::CleanAudioFrame& clean, audio::CaptureFrame* frame) noexcept;
  /**
   * @brief 根据播放参考、预热和尾音窗口更新本帧的 near_voice 候选。
   *
   * 每次 Detect 成功后对同一帧调用一次，计数以帧推进。只修改 near_voice，
   * 保留本帧 wake 与 VAD 标志；候选仍需 VoiceAudio 确认，不能直接作为打断事件。
   * @return 成功返回 true；若窗口结束时 VAD 复位失败，返回 false 并停止发布本帧。
   */
  bool GateNearVoice(const PlaybackState& playback, audio::CaptureFrame* frame) noexcept;
  /// 释放库资源并清空检测、播放门控和错误状态；允许重复调用。
  void Close() noexcept;
  /// 最近一次失败的静态文本；普通成功调用不清除旧错误，Open 成功或 Close 时清空。
  const char* LastError() const noexcept {
    return error_;
  }

 private:
  /// 同时复位库内 VAD 历史、模式和外部语句计数；不修改当前已输出帧的标志。
  bool ResetVad() noexcept;
  /// reason 必须具有静态存储期；仅保存指针供后端读取，不在此分配错误字符串。
  bool Fail(const char* reason) noexcept;

  // 句柄由本对象独占；Snowboy 通过 C bridge 隔离旧 C++ ABI。
  BoompiSnowboyLegacyHandle* snowboy_{nullptr};
  VadInst* vad_{nullptr};
  // 连续人声/非人声的累计毫秒数，在各自的起止门限处饱和。
  std::uint32_t vad_speech_ms_{0U}, vad_silence_ms_{0U};
  // 预热与尾音抑制互斥，共用剩余帧数；减至零的当帧仍抑制并复位 VAD。
  unsigned suppress_remaining_{0U};
  /// 已开始非零且未静音的渲染、但尚无采集参考时的帧计数，用于缺参考告警。
  unsigned reference_wait_frames_{0U};
  /// 已越过开口确认门限，尚未达到连续静音收尾门限。
  bool vad_in_speech_{false};
  // 活动轮次用于断点恢复；armed 表示仍在等待本轮播放的真实参考。
  bool playback_session_active_{false}, aec_warmup_armed_{false};
  /// 零增益播放暂时放行近讲；恢复可听渲染时重新启用参考等待。
  bool silent_playback_bypass_{false};
  const char* error_{""};
};

}  // namespace boompi::platform::rv1106
