/**
 * @file device_ui.h
 * @brief 定义 application 与 RV1106 板端 UI 子系统之间的线程安全边界。
 *
 * 公开接口只交换语义状态、字幕、音量和离散动作；LVGL 对象、触摸坐标、摄像头
 * 缓冲以及硬件文件描述符全部隐藏在实现中，由各自 owner 管理。
 */
#pragma once

#include <cstdint>
#include <string_view>

namespace boompi::ui {

/**
 * @brief application 发布给板端 UI 的语音交互状态。
 *
 * 该枚举只描述显示语义，不复制对话状态机。application 仍然负责 turn、
 * epoch、VAD 和打断判定，UI 根据这里的状态选择表情、标题与可点击行为。
 */
enum class DeviceUiState : std::uint8_t {
  kIdle, kListening, kThinking, kSpeaking, kHappy, kOffline, kError,
  // response.done 后仍有尾播，需要继续允许触屏打断。
  kSpeakingTail,
};

/** @brief UI 线程交还给 application 的离散用户意图。 */
enum class DeviceUiAction : std::uint8_t { kWake, kInterrupt };

/**
 * @brief RV1106 显示、触摸、摄像头预览与音量交互的板端入口。
 *
 * application 可以从自己的 actor 线程发布状态和字幕，也可以轮询触摸动作与
 * 音量变化。实现内部把这些数据压缩成小型快照，由唯一 UI worker 创建、访问和
 * 销毁全部 LVGL 对象，避免 LVGL 的非线程安全对象跨线程流动。camera worker 只
 * 生产固定尺寸 RGB565 帧，最终的图像提交仍在 UI worker 中完成。
 *
 * Open()/Close() 拥有 GPIO、SPI、I2C、UI 线程和摄像头线程的完整生命周期。
 */
class DeviceUi final {
 public:
  DeviceUi() noexcept = default;
  ~DeviceUi() noexcept;
  DeviceUi(const DeviceUi&) = delete;
  DeviceUi& operator=(const DeviceUi&) = delete;

  /**
   * @brief 读取持久化音量，并把缺失或非法配置收敛到给定默认值。
   * @param fallback 配置不可用时采用的百分比，返回前限制到 0..100。
   */
  static std::uint8_t LoadVolume(std::uint8_t fallback) noexcept;

  /**
   * @brief 初始化板端外设并启动独占 LVGL 的 UI worker。
   * @return 面板、触摸、字体和 LVGL 页面全部就绪时返回 true。
   */
  bool Open() noexcept;

  /** @brief 发布语音显示状态；实际 LVGL 更新由 UI worker 异步完成。 */
  void SetState(DeviceUiState state) noexcept;

  /** @brief 发布两行对话字幕；字符串在调用返回前复制进共享快照。 */
  void SetText(std::string_view first_line, std::string_view second_line) noexcept;

  /**
   * @brief 取出最近一次未消费的唤醒或打断动作。
   * @return 有新动作写入 action 时返回 true，同一动作只交付一次。
   */
  bool PollAction(DeviceUiAction* action) noexcept;

  /**
   * @brief 取出滑块产生的最近音量百分比。
   *
   * 拖动预览和释放提交都通过此入口更新实时播放增益；配置文件只在 UI worker
   * 收到提交事件时写入。
   */
  bool PollVolumeChange(std::uint8_t* percent) noexcept;

  /** @brief 发布当前音量，使页面重建后仍显示与音频引擎一致的值。 */
  void SetVolume(std::uint8_t percent) noexcept;

  /** @brief 停止后台工作、熄灭背光并释放所有板端文件描述符。 */
  void Close() noexcept;

 private:
  struct Impl;
  Impl* impl_{nullptr};
};

}  // namespace boompi::ui
