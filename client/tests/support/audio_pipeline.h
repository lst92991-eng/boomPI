/**
 * @file audio_pipeline.h
 * @brief Host 回归中替换板端 I/O 的脚本后端，由 CMake 的 include 顺序选择。
 *
 * 测试主线程 PushCapture → 引擎采集线程 Read/Process → 真实 AudioTasks 队列；
 * 真实播放线程 Render/Drain → RenderCallsSnapshot 验证结果。这里不执行声学算法，
 * 算法连接在 audio_pipeline_test.cpp 中通过真实生产模块另行验证。
 */
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "boompi/audio/audio_frames.h"

namespace boompi::test::audio_pipeline {

/// 记录进入后端的 16 kHz PCM 和增益，验证入队顺序、短尾长度与实时音量。
struct RenderCall final {
  std::vector<std::int16_t> pcm;
  float gain{0.0F};
};

/// 选择阻塞位置，以验证 Drop/Close 能否唤醒正在渲染或排空的播放线程。
enum class PlaybackBlock { None, Render, Drain };
/// 后续进入选定阶段时阻塞，直到产品发出中断或测试的一秒兜底截止时间到达。
void BlockPlayback(PlaybackBlock stage) noexcept;
/// 下一次 PreparePlayback 返回失败；由 Reset 清除注入状态。
void FailPlaybackPreparation() noexcept;
/// 等待播放线程确实进入阻塞点，避免把尚未开始的 I/O 当作已中断。
bool WaitForPlaybackBlocked(std::chrono::milliseconds timeout) noexcept;
/// 核对采集线程先 Arm、另一个播放线程再 Prepare/Render/Drain 的轮次与所有权。
bool PlaybackOwnerOrderIsValid() noexcept;

/// 每个场景启动前清空共享状态；只能在上一场景的引擎线程已退出后调用。
void Reset() noexcept;
/// 放入预先判定的采集帧并唤醒采集线程，容器容量仅属于测试驱动器。
void PushCapture(const audio::CaptureFrame& frame) noexcept;
// 以下等待都以至少达到 count 为成功条件；timeout 单位为毫秒，超时返回 false。
bool WaitForCaptureReads(std::size_t count, std::chrono::milliseconds timeout) noexcept;
bool WaitForProcessedFrames(std::size_t count, std::chrono::milliseconds timeout) noexcept;
bool WaitForRenderCalls(std::size_t count, std::chrono::milliseconds timeout) noexcept;
bool WaitForCaptureInterrupts(std::size_t count, std::chrono::milliseconds timeout) noexcept;
/// 在共享锁内复制已记录的渲染调用，避免测试读取正在写入的容器。
std::vector<RenderCall> RenderCallsSnapshot();

}  // namespace boompi::test::audio_pipeline

namespace boompi::platform::rv1106 {

using audio::CaptureFrame;
using audio::RawCaptureFrame;

/**
 * @brief 与产品 AudioPipeline 同名的最小测试 I/O 实现。
 * API 保持相同调用方向，内部改为条件变量等待和记录；测试替身不打开硬件设备。
 */
class AudioPipeline final {
 public:
  AudioPipeline() noexcept = default;
  ~AudioPipeline() noexcept;
  AudioPipeline(const AudioPipeline&) = delete;
  AudioPipeline& operator=(const AudioPipeline&) = delete;

  /// 建立共享打开状态；配置由测试准备，不校验板端设备路径或模型。
  bool Open() noexcept;
  /// 采集线程等待注入帧，保存为 pending；关闭/中断返回 false。
  bool ReadCapture20ms(RawCaptureFrame* raw) noexcept;
  /// 将 pending 的判定结果交给引擎；每次 Read 结果只能处理一次。
  bool ProcessCapture20ms(const RawCaptureFrame& raw, CaptureFrame* frame) noexcept;
  /// 此替身不持有声学历史，因此直接成功；引擎自己的复位逻辑仍照常运行。
  bool ResetListener() noexcept;
  /// 记录采集线程的播放准备请求，供后续 Prepare 检查轮次顺序。
  bool ArmPlayback() noexcept;
  /// 记录播放线程身份，并消费已注入的准备失败条件。
  bool PreparePlayback() noexcept;
  /// 复制 PCM/gain 供断言，模拟 20 ms 媒体推进；可在进入阶段后阻塞。
  bool Render20ms(const std::int16_t* pcm16, std::size_t samples, float gain) noexcept;
  /// 可阻塞的排空点，不执行真实声卡 drain。
  bool DrainPlayback() noexcept;
  /// 由播放线程结束当前准备状态；跨线程唤醒走 InterruptPlayback。
  void DropPlayback() noexcept;
  /// 唤醒等待注入输入的采集线程，使 Read 返回 false。
  void InterruptCapture() noexcept;
  /// 唤醒 Render/Drain 的阻塞点，不代替播放 owner 执行 Drop。
  void InterruptPlayback() noexcept;
  /// 测试后端不记录细节错误，返回空串；用于观察引擎自身的错误分类。
  std::string LastError() const;
  /// 发布关闭状态并唤醒所有条件变量等待，供引擎 join 工作线程。
  void Close() noexcept;

 private:
  struct Impl;
  Impl* impl_{nullptr};
};

}  // namespace boompi::platform::rv1106
