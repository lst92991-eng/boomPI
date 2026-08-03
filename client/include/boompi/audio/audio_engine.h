#ifndef BOOMPI_AUDIO_AUDIO_ENGINE_H_
#define BOOMPI_AUDIO_AUDIO_ENGINE_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace boompi::audio {

constexpr std::size_t kVoiceFrameSamples16k = 320U;
using VoiceFrame16k = std::array<std::int16_t, kVoiceFrameSamples16k>;

/// @brief 一帧经过双麦 3A、Snowboy 和 VAD 后交给 application 的结果。
///
/// 帧长固定为 20 ms/16 kHz/mono。`sequence` 在本进程内单调递增；一旦 ALSA、
/// 采集或重采样失去连续性时，`discontinuity` 置位，application 必须取消当前轮次。
/// `near_voice` 来自 Rockchip 3A 后 WebRTC VAD，不读取 vendor `wakeup_status`；
/// Mode1 参考首次出现后的 600 ms AEC 预热和自然播放结束后的 300 ms 尾音窗
/// 会强制为 false。主动打断不复位正在进行的人声 VAD，保证短打断词仍能产生结束事件。
struct CaptureFrame final {
  VoiceFrame16k pcm{};
#ifdef BOOMPI_AEC_LOOP_DIAGNOSTICS
  // HIL-only snapshot of the exact 3A input planes. Production builds do not
  // carry or copy these samples.
  std::array<VoiceFrame16k, 4U> aec_input{};
#endif
  std::uint64_t sequence{0U};
  bool wake{false}, vad_now{false}, vad_started{false}, vad_ended{false};
  bool discontinuity{false}, near_voice{false}, reference_active{false};
};

/// @brief 板端音频引擎的启动配置。
///
/// 配置只在 `Open` 时复制；capture/playback 热路径不读取环境变量，也不创建 ALSA 或模型资源。
struct AudioEngineConfig final {
  std::string capture_pcm, playback_pcm;
  std::string snowboy_resource, snowboy_model, snowboy_sensitivity{"0.5"};
  float snowboy_gain{1.0F};
  float playback_gain{1.8F}, duck_gain{0.0F};
  std::int8_t left_polarity{1}, right_polarity{1};
  std::int32_t vad_mode{3}; std::uint16_t vad_start_ms{120U}, vad_end_ms{700U};
};

/// @brief 对话业务使用的音频门面。
///
/// application 层只看到固定 20 ms 帧和播放控制。内部唯一播放线程消费有界 TTS 环形队列；
/// AEC 使用 Codec Mode1 同步采集的 REF-L；REF-R 仍保留在四通道采集数据中。Close 会先唤醒并 join
/// 播放线程，随后才释放 platform 层的 ALSA、模型、DSP 和临时 mixer 配置。
class AudioEngine final {
 public:
  AudioEngine() noexcept = default;
  ~AudioEngine() noexcept;
  AudioEngine(const AudioEngine&) = delete;
  AudioEngine& operator=(const AudioEngine&) = delete;

  /// @brief 一次性打开 ALSA、重采样器、Rockchip 3A、Snowboy、VAD，并启动唯一播放线程。
  /// @return 成功后才能调用其余方法；失败时对象仍可安全 `Close`，原因由 `last_error` 返回。
  bool Open(const AudioEngineConfig& config) noexcept;

  /// @brief 阻塞采集并处理一个 20 ms 帧。
  ///
  /// 只能由 application actor 调用；一次读取同步的 `[mic0,mic1,refL,refR]` period。
  bool Capture(CaptureFrame* frame) noexcept;

  /// @brief 清空 Snowboy/VAD 的当前判定历史，不停止持续采集或 Rockchip 3A。
  bool ResetListener() noexcept;

  /// @brief 按服务端 sequence 把 little-endian 24 kHz mono PCM 复制到 1.5 秒有界环。
  /// @return 队列关闭、满、序号断裂或参数非法时返回 false；绝不覆盖尚未播放的帧。
  bool QueueTts24k(const std::uint8_t* pcm_bytes, std::size_t byte_count,
                   std::uint64_t sequence) noexcept;

  /// @brief 开始一个新的 TTS 流并复位播放重采样相位；必须先于首个 PCM 包调用。
  bool BeginPlayback() noexcept;

  /// @brief 标记当前 TTS 已收完；播放线程会消费最后一个短帧、drain ALSA，再置完成状态。
  bool EndPlayback() noexcept;

  /// `Duck` 只调整后续渲染增益；`DropPlayback` 异步唤醒播放线程并清空尚未渲染的 TTS。
  void Duck(bool enabled) noexcept;
  void DropPlayback() noexcept;

  /// 前两个查询是无锁快照；`playback_failed` 只表示最近一次已结束的流发生硬件/渲染失败，
  /// 用户主动 drop 不计为失败。BeginPlayback 会清除此状态。
  bool playing() const noexcept; bool playback_done() const noexcept;
  bool playback_failed() const noexcept;

  /// 返回 AudioEngine 或板级后端最近一次错误；允许 application 与播放线程并发读取。
  std::string last_error() const;

  /// 幂等停止：先唤醒/打断 ALSA 播放并 join，再释放 DSP、模型、重采样器和 PCM。
  void Close() noexcept;

 private:
  struct Impl;
  Impl* impl_{nullptr};
};

}  // namespace boompi::audio

#endif  // BOOMPI_AUDIO_AUDIO_ENGINE_H_
