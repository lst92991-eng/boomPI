/// @file 采集帧、音频队列和播放生命周期。
#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>

#include "boompi/audio/board_voice_profile.h"

namespace boompi::audio {

/// 16 kHz 音频的 20 ms 帧长。整个客户端以这一个时间单位推进 VAD、唤醒和上传时序。
inline constexpr std::size_t kVoiceFrameSamples16k = kVoiceFrameSamples;
using VoiceFrame16k = std::array<std::int16_t, kVoiceFrameSamples16k>;

/// @brief 一帧经过双麦 3A、Snowboy 和 VAD 后交给 VoiceAudio 的结果。
///
/// 帧长固定为 20 ms/16 kHz/mono。`sequence` 在本进程内单调递增；一旦 ALSA、
/// 采集或重采样失去连续性时，`discontinuity` 置位，当前轮次必须取消。
/// `near_voice` 来自 Rockchip 3A 后 WebRTC VAD，不读取 vendor `wakeup_status`；
/// Mode1 参考首次出现后的 600 ms AEC 预热和自然播放结束后的 300 ms 尾音窗
/// 会强制为 false。主动打断不复位正在进行的人声 VAD，保证短打断词仍能产生结束事件。
struct CaptureFrame final {
  VoiceFrame16k pcm{};
  /// `sequence` 标识 application 实际收到的处理帧；序号空洞表示队列边界已丢失连续性。
  std::uint64_t sequence{0U};
  /// 该 PCM 对应采集 period 结束时的 steady-clock 时间；随 3A 固定一帧延迟一起延后。
  std::uint64_t timestamp_us{0U};
  /// `input_dbfs` 取双路原始麦克风中较大值，负责 VAD 准入；`voice_dbfs` 来自 AEC 后单声道，
  /// 负责播放期间的近讲判断。两者用途不同，调参时需要分别观察。
  float input_dbfs{-120.0F}, voice_dbfs{-120.0F};
  /// `vad_now` 是当前帧电平；started/ended 是带 120/700 ms 滞回后的边沿事件。
  bool wake{false}, vad_now{false}, vad_started{false}, vad_ended{false};
  /// `actor_overrun` 是 `discontinuity` 的一个具体来源：application 超过 80 ms 未消费。
  bool discontinuity{false}, actor_overrun{false};
  /// `near_voice` 已应用 AEC 预热/尾音抑制；`reference_active` 与本帧 DSP 输出处于同一时间轴。
  bool near_voice{false}, reference_active{false};
};

/// @brief application 等待下一帧的结果，区分正常轮询超时和音频故障。
enum class CaptureResult : std::uint8_t {
  kFrame,    ///< 已返回一帧，调用者取得该帧所有权。
  kTimeout,  ///< 本次等待期内无帧，音频链路仍可继续工作。
  kFailed,   ///< 生命周期错误或采集线程失败，应读取 `last_error` 并结束音频循环。
};

/// @brief TTS PCM 入队结果；瞬态流状态不写入硬件 `last_error`。
enum class QueueTtsResult : std::uint8_t {
  kQueued,           ///< PCM 已完整复制进有界环。
  kNotOpen,          ///< AudioEngine 尚未打开或已经关闭。
  kInvalidArgument,  ///< 空包、空指针或奇数字节，无法组成 S16_LE sample。
  kNotActive,        ///< 还未开始当前播放会话。
  kEnding,           ///< 已收到流结束标记，当前会话不再接受 PCM。
  kFull,             ///< 1.5 秒容量不足；调用方需要把它当作明确背压处理。
  kDiscontinuous,    ///< 服务端 PCM sequence 出现空洞，继续播放会掩盖音频缺失。
};

/// @brief 板端音频引擎的启动配置。
///
/// 配置只在 `Open` 时复制；capture/playback 热路径不读取环境变量，也不创建 ALSA 或模型资源。
struct AudioEngineConfig final {
  /// ALSA PCM 名称由板端配置给出，便于匹配不同镜像中的 card/device 编号。
  std::string capture_pcm{kCapturePcm}, playback_pcm{kPlaybackPcm};
  /// Snowboy 模型资源在 Open 时交给旧 ABI bridge，实时线程只保留已创建句柄。
  std::string snowboy_resource{kSnowboyResource};
  std::string snowboy_model{kSnowboyModel};
  std::string snowboy_sensitivity{kBoardVoiceProfile.wake_sensitivity};
  /// 默认播放增益与原始麦克风 VAD 准入门限；运行中音量可经原子值更新。
  float playback_gain{1.0F};
  float vad_min_dbfs{kBoardVoiceProfile.vad_admission_dbfs};
  /// 麦克风焊接极性只能取 +1/-1；数字回采参考不应用此设置。
  std::int8_t left_polarity{kBoardVoiceProfile.left_polarity};
  std::int8_t right_polarity{kBoardVoiceProfile.right_polarity};
};

/// @brief 管理采集、播放两条线程及其有界队列。
///
/// VoiceAudio 消费处理后的采集帧，并提交 TTS。高优先级采集线程持续读取 ALSA 并执行 3A，
/// 次高优先级播放线程消费 TTS 环形队列。
/// AEC 使用 Codec Mode1 同步采集的 REF-L；REF-R 仍保留在四通道采集数据中。Close 会先唤醒并 join
/// 两条音频线程，随后才释放 platform 层的 ALSA、模型和 DSP。
class AudioEngine final {
 public:
  AudioEngine() noexcept = default;
  ~AudioEngine() noexcept;
  AudioEngine(const AudioEngine&) = delete;
  AudioEngine& operator=(const AudioEngine&) = delete;

  /// @brief 打开音频资源并启动独立 capture/playback 实时线程。
  /// @return 成功后才能调用其余方法；失败时对象仍可安全 `Close`，原因由 `last_error` 返回。
  bool Open(const AudioEngineConfig& config) noexcept;

  /// @brief 在给定时间内取得一个由采集线程处理完成的 20 ms 帧。
  ///
  /// 只能由 application actor 调用；超时不代表 ALSA 故障，调用者应借此观察
  /// stop flag。该边界保证 ALSA 阻塞时，进程仍能进入 Close 并调用 snd_pcm_abort。
  CaptureResult Capture(CaptureFrame* frame, std::chrono::milliseconds timeout) noexcept;

  /// @brief 清空 Snowboy/VAD 的当前判定历史，不停止持续采集或 Rockchip 3A。
  bool ResetListener() noexcept;

  /// @brief 按服务端 sequence 把 little-endian 24 kHz mono PCM 复制到 1.5 秒有界环。
  /// @return 明确区分生命周期、容量、序号和参数错误；绝不覆盖尚未播放的帧。
  QueueTtsResult QueueTts24k(const std::uint8_t* pcm_bytes, std::size_t byte_count,
                             std::uint64_t sequence) noexcept;

  /// @brief 开始新 TTS 流；先等采集线程在帧边界武装 AEC 门控。
  ///
  /// 必须先于首个 PCM 包调用。播放线程在首次渲染前准备自己的 PCM/重采样器，
  /// 帧边界握手保证 capture 侧先武装 AEC，随后播放才可能产生硬件参考。
  bool BeginPlayback() noexcept;

  /// @brief 标记当前 TTS 已收完；播放线程会消费最后一个短帧、drain ALSA，再置完成状态。
  bool EndPlayback() noexcept;

  /// `SetPlaybackGain` 保存用户音量，`SetPlaybackScale` 叠加会话内临时衰减；两者只影响
  /// 后续渲染。`DropPlayback` 异步唤醒播放线程并清空尚未渲染的 TTS，用于打断旧回复。
  void SetPlaybackGain(float gain) noexcept;
  void SetPlaybackScale(float scale) noexcept;
  void DropPlayback() noexcept;

  /// 三个查询都是无锁快照。`playback_active` 表示 BeginPlayback 已开始且 drain/drop 尚未完成，
  /// 不等同于扬声器此刻必然出声；用户主动 drop 不计为 `playback_failed`。
  bool playback_active() const noexcept {
    return !playback_done();
  }
  bool playback_done() const noexcept;
  bool playback_failed() const noexcept;

  /// 返回 AudioEngine 或板级后端最近一次错误；允许 application 与播放线程并发读取。
  std::string last_error() const;

  /// 幂等停止：先唤醒并 join capture/playback，再释放 DSP、模型、重采样器和 PCM。
  void Close() noexcept;

 private:
  struct Impl;
  Impl* impl_{nullptr};
};

}  // namespace boompi::audio
