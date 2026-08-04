#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace boompi::platform::rv1106 {

#ifndef BOOMPI_ROCKCHIP_REFERENCE_CHANNELS
#define BOOMPI_ROCKCHIP_REFERENCE_CHANNELS 1
#endif
static_assert(BOOMPI_ROCKCHIP_REFERENCE_CHANNELS == 1 ||
                  BOOMPI_ROCKCHIP_REFERENCE_CHANNELS == 2,
              "Rockchip 3A supports one or two reference channels here");

constexpr std::size_t kRockchipVoiceFrameSamples16k = 320U;
using RockchipVoiceFrame16k =
    std::array<std::int16_t, kRockchipVoiceFrameSamples16k>;

/// @brief 当前板卡唯一的 Rockchip 语音 3A 适配器。
///
/// capture/DSP 主线程独占本对象，并传入同一个 Codec Mode1 period 的 20 ms 平面。
/// 生产 vendor 输入固定为 `[mic-left,mic-right,ref-left]`，每个 sample 只处理一次。
/// Codec Mode1 仍采集 ref-right；Process 保留该参数，但生产配置不会把它送入 vendor。
///
/// 产品帧是 320 samples，vendor 每次只吃 256 samples。两个固定 FIFO 跨调用拼接，
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
  /// 初始化失败后实例保持关闭；重复打开返回 kAlreadyOpen 且不改变已有句柄。Close 幂等释放
  /// 句柄、参数树和 FIFO 状态，可在初始化失败后调用。
  /// @return 成功、重复打开或具体初始化阶段的状态码。
  bool Open() noexcept;
  void Close() noexcept;  ///< 幂等关闭，允许在 Open 失败后调用。

  /// @brief 将双麦与同步硬件参考送入组合 AEC/STDT/BF/ANR 路径。
  ///
  /// @param mic_left 16 kHz/S16/mono 左麦 320 samples，即 20 ms。
  /// @param mic_right 16 kHz/S16/mono 右麦 320 samples，即 20 ms。
  /// @param reference_left Codec Mode1 左播放参考，16 kHz/S16/mono/320 samples。
  /// @param reference_right Codec Mode1 右播放参考，16 kHz/S16/mono/320 samples；
  ///        生产单参考配置忽略它，仅供双参考 HIL 配置使用。
  /// @param output 非空输出帧。除空指针错误外，函数先清零该帧；成功时写入 320 samples。
  /// @return FIFO 或 backend 处理失败时实例会自动 Close，调用方必须重新 Open；参数错误、
  ///         未打开状态不会改变实例生命周期。
  bool Process(
      const RockchipVoiceFrame16k& mic_left,
      const RockchipVoiceFrame16k& mic_right,
      const RockchipVoiceFrame16k& reference_left,
      const RockchipVoiceFrame16k& reference_right,
      RockchipVoiceFrame16k* output) noexcept;
  /// @brief 返回调用线程观察到的句柄状态；本类不提供跨线程同步。
  bool is_open() const noexcept { return handle_ != nullptr; }

 private:
  static constexpr std::size_t kVendorBlockSamples = 256U;
  static constexpr std::size_t kVendorInputChannels =
      2U + BOOMPI_ROCKCHIP_REFERENCE_CHANNELS;
  static constexpr std::size_t kInputFifoFrames = 512U;
  static constexpr std::size_t kOutputFifoSamples = 640U;

  void ResetFifos(bool prime_output) noexcept;

  void* handle_{nullptr};
  void* parameters_{nullptr};
  std::array<std::int16_t, kInputFifoFrames * kVendorInputChannels>
      input_fifo_{};
  std::array<std::int16_t, kOutputFifoSamples> output_fifo_{};
  std::array<std::int16_t, kVendorBlockSamples> vendor_output_{};
  std::size_t input_count_{0U};
  std::size_t output_count_{0U};
};

}  // namespace boompi::platform::rv1106
