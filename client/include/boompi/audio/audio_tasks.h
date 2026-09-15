#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>

#include "boompi/audio/audio_frames.h"

namespace boompi::audio {

// 取帧超时不等于硬件故障；Failed 时停止当前音频流程。
enum class CaptureResult : std::uint8_t { Frame, Timeout, Failed };
// 入队结果明确区分背压、缺帧和调用状态，不把暂时满队列写成设备错误。
enum class QueueTtsResult : std::uint8_t {
  Queued,
  NotOpen,
  InvalidArgument,
  NotActive,
  Ending,
  Full,
  Discontinuous,
};

// 两个音频任务及其队列。主线程通过 VoiceAudio 使用本类，不直接操作声卡。
// ReadMicrophoneTask：声卡 → 音频处理 → 80 ms 采集队列。
// PlaySpeakerTask：1.5 s 回复队列 → 重采样/音量 → 声卡。
class AudioTasks final {
 public:
  AudioTasks() noexcept = default;
  ~AudioTasks() noexcept;
  AudioTasks(const AudioTasks&) = delete;
  AudioTasks& operator=(const AudioTasks&) = delete;

  // 打开声卡、算法并启动两个任务。失败可 Stop，原因见 LastError。
  bool Start(float playback_gain = 1.0F) noexcept;
  // 取处理后的 20 ms 帧。等待最多 timeout，主线程借超时继续处理退出等操作。
  CaptureResult ReadProcessedFrame(CaptureFrame* frame,
                                   std::chrono::milliseconds timeout) noexcept;
  // 在采集帧边界复位唤醒/VAD，最多等 100 ms；保留已有 PCM，仅清旧判定。
  bool ResetListener() noexcept;

  // 每轮首包前调用；先由采集任务武装 AEC，再允许播放任务出声。
  bool BeginPlayback() noexcept;
  // 复制一包 16 kHz/mono/S16_LE，1～320 样本；短包只能是末包，序号必须连续。
  // Full 时调用方终止本轮，不能覆盖旧帧或跳过一包继续播放。
  QueueTtsResult QueueReplyFrame(const std::uint8_t* pcm_bytes, std::size_t byte_count,
                                 std::uint64_t sequence) noexcept;
  // 声明不会再有新包；队列与声卡尾音由播放任务播完后再发布完成。
  bool EndPlayback() noexcept;
  // 丢弃排队的回复并中断可能阻塞的 write/drain；收尾完成由播放任务发布。
  void DropPlayback() noexcept;

  // 用户音量与插话探测的临时衰减分别保存，只影响之后渲染的帧。
  void SetPlaybackGain(float gain) noexcept;
  void SetPlaybackScale(float scale) noexcept;  // 限制到 0～1，播完或 drop 后恢复 1。
  bool IsPlaybackDone() const noexcept;
  bool HasPlaybackFailed() const noexcept;  // 与 done 一起读，才是本轮最终结果。
  std::string LastError() const;

  // 唤醒阻塞 I/O、等待两个任务退出，再释放声卡和算法。可重复调用。
  void Stop() noexcept;

 private:
  struct Impl;
  Impl* impl_{nullptr};
};

}  // namespace boompi::audio
