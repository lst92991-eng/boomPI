/**
 * @file rockchip_voice_dsp.h
 * @brief 将产品 20 ms 帧适配到 Rockchip librkaudio 固定分块 ABI。
 *
 * AudioPipeline 在联合降采样后调用 Process，输出交给 SpeechDetector。此公共边界
 * 只暴露自己的帧结构和不透明句柄，vendor 参数类型限制在对应源文件。
 */
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "boompi/audio/audio_frames.h"

namespace boompi::platform::rv1106 {

inline constexpr std::size_t kRockchipVoiceFrameSamples16k = audio::kVoiceFrameSamples;
/// @brief Rockchip 语音 3A 参数配置及 256/320 sample 分块转换。
///
/// capture/DSP 主线程独占本对象，并传入同一个 Codec Mode1 period 的 20 ms 平面。
/// vendor 输入固定为 `[mic-left,mic-right,ref-left]`，每个 sample 只处理一次。
/// Codec Mode1 仍采集 ref-right，因左右播放内容相同，只使用 ref-left。
///
/// 产品帧是320 samples，本适配器每次送vendor 256 samples。一个输入块跨调用补齐，
/// 不补零、不丢输入；Open 先放入一帧静音输出，所以稳定后每次 Process 恰好返回
/// 320 个 mono samples，并带固定一帧启动延迟。Process 不进行动态分配。
class RockchipVoiceDsp final {
 public:
  RockchipVoiceDsp() noexcept = default;
  ~RockchipVoiceDsp() noexcept;
  RockchipVoiceDsp(const RockchipVoiceDsp&) = delete;
  RockchipVoiceDsp& operator=(const RockchipVoiceDsp&) = delete;

  /// @brief 分配 vendor 参数树、创建句柄并 prime 固定输出 FIFO。
  ///
  /// 初始化失败后实例保持关闭；重复打开返回 false 且不改变已有句柄。Close 幂等释放
  /// 句柄、参数树和 FIFO 状态，可在初始化失败后调用。
  /// @return 完整初始化成功返回 true，已打开或任一步失败返回 false。
  bool Open(int delay_samples = 0) noexcept;
  /// 必须避开 Process；AudioPipeline 只在启动、采集断点或线程退出后调用生命周期接口。
  void Close() noexcept;  ///< 幂等关闭，允许在 Open 失败后调用。

  /// @brief 将双麦与同步硬件参考送入组合 AEC/STDT/BF/ANR 路径。
  ///
  /// @param input 同一采集帧的双麦、参考及元数据，每个声道320样本。
  /// @param output 非空输出；PCM与电平/参考/时间戳一起延迟一帧，首帧使用静音元数据。
  /// @return 成功交付320样本；失败时调用方终止本次采集，不将无效数据送入检测。
  bool Process(const audio::CaptureChannels& input, audio::CleanAudioFrame* output) noexcept;
  /// @brief 返回调用线程观察到的句柄状态；本类不提供跨线程同步。
  bool IsOpen() const noexcept {
    return handle_ != nullptr;
  }

 private:
  static constexpr std::size_t kVendorBlockSamples = 256U;
  static constexpr std::size_t kVendorInputChannels = 3U;
  /// 输出 FIFO 覆盖一帧预置静音和 vendor 分块产生的最大暂存量。
  static constexpr std::size_t kOutputFifoSamples = 640U;

  /// 清除所有跨调用余数；`prime_output` 为 Open 建立固定的一帧算法延迟。
  void ResetFifos(bool prime_output) noexcept;

  /// vendor 句柄与参数树生命周期绑定，只有 capture 线程可以访问。
  void* handle_{nullptr};
  void* parameters_{nullptr};
  /// 输入按 `[mic-left,mic-right,ref-left]` 逐 sample 交错保存。
  std::array<std::int16_t, kVendorBlockSamples * kVendorInputChannels> input_block_{};
  std::array<std::int16_t, kOutputFifoSamples> output_fifo_{};
  std::size_t input_count_{0U};
  std::size_t output_count_{0U};
  /// 和输出 FIFO 的一帧启动延迟对应；参考、原始电平和 PCM 必须描述同一时间片。
  audio::CaptureMetadata previous_metadata_{};
};

}  // namespace boompi::platform::rv1106
