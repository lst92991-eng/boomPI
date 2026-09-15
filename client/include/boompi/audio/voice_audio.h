#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "boompi/audio/audio_format.h"

namespace boompi::audio {

// 唤醒后按 VAD 开口；追问需要连续 400 ms 近讲。等待窗口由 应用模块 计时。
enum class ListenMode : std::uint8_t { Wake, FollowUp };
enum class AudioEventKind : std::uint8_t {
  Wake,
  SpeechStart,
  Pcm,
  Barge,
  PlaybackDone,
  Fault,
};

// 每个事件拥有自己的数据。开始事件总在对应 PCM 之前，便于应用先分配轮次。
struct AudioEvent final {
  AudioEventKind kind{AudioEventKind::Fault};
  // Barge 是被打断的旧轮次；PlaybackDone/播放故障是对应回复的轮次。
  std::uint32_t generation{0U};
  std::array<std::int16_t, kVoiceFrameSamples> pcm{};
  // Pcm 保留采集时的本地序号、单调时钟微秒值；不是网络包序号或 UTC。
  std::uint64_t sequence{0U};
  std::uint64_t timestamp_us{0U};
  bool end{false};  // 这帧 PCM 上传成功后结束输入。
};

// 应用线程独占语句历史与插话处理；AudioTasks 内部负责采集和播放线程。
// 使用顺序：Open → 持续 ProcessEvents，按需 Listen/Play → Close。
class VoiceAudio final {
 public:
  VoiceAudio() noexcept = default;
  ~VoiceAudio() noexcept;
  VoiceAudio(const VoiceAudio&) = delete;
  VoiceAudio& operator=(const VoiceAudio&) = delete;

  // 打开声卡和算法；volume 限制到 0～100，重复打开先关闭旧实例。
  bool Open(std::uint8_t volume = 60U);
  // 推进采集并直接返回一批事件；无事件也要持续调用。
  // 每次先清空 events，调用方跨循环复用。最多等待 timeout，不含处理耗时。
  // 不完整的输入以 Fault 替换；开始事件和句首 PCM 在同一批按顺序交付。
  void ProcessEvents(std::vector<AudioEvent>& events, std::chrono::milliseconds timeout);
  // 清句首历史、在采集帧边界复位检测，再开始等人声；活动播放期间返回 false。
  bool Listen(ListenMode mode);

  // 复制一包 16 kHz/mono/S16_LE 回复；bytes 为偶数，非末包 640 字节，末包 2～640。
  // generation 非零且属于当前回复；sequence 从 0 连续递增，首包同时带 start。
  // end 表示已收完，PlaybackDone 才表示实际尾播完成。失败应取消整轮，不跳包续播。
  bool Play(std::uint32_t generation, const std::uint8_t* pcm, std::size_t bytes,
            std::uint32_t sequence, bool start, bool end);
  // 异步清队列并中断播放；持续采集保留，用于继续检测人声。
  void StopPlayback();
  // 取消录音及句首历史；调用方应丢弃手中剩余的本轮事件，不影响当前播放。
  void CancelInput();
  void SetVolume(std::uint8_t volume);

  // 可恢复缺帧仍为 healthy；不可恢复的设备/算法失败为 false。
  bool IsHealthy() const;
  std::string LastError() const;
  // 先等工作线程退出再释放资源；允许重复调用。
  void Close() noexcept;

 private:
  struct Impl;
  Impl* impl_{nullptr};
};

}  // namespace boompi::audio
