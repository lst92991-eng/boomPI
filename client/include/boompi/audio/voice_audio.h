#pragma once

/**
 * @file voice_audio.h
 * @brief 对话层使用的录音、播放和语音事件接口。
 *
 * 板级采样格式、VAD、500 ms pre-roll、AEC 打断探针和播放缓冲全部留在实现内。
 * application 只按顺序消费语义事件和固定 20 ms PCM，不接触声学标定参数。
 */

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>

#include "boompi/audio/audio_format.h"

namespace boompi::audio {

enum class AudioEventKind : std::uint8_t {
  Wake,
  SpeechStart,
  Pcm,
  Barge,
  PlaybackDone,
  Fault,
};

struct AudioEvent final {
  AudioEventKind kind{AudioEventKind::Fault};
  /// Barge 指向被打断的回复；PlaybackDone 和播放故障指向对应回复。
  std::uint32_t generation{0U};
  std::array<std::int16_t, kVoiceFrameSamples> pcm{};
  std::uint64_t sequence{0U};
  std::uint64_t timestamp_us{0U};
  bool end{false};  ///< Pcm 的末帧，帧内声音仍需先上传再结束本轮。
};

class VoiceAudio final {
 public:
  VoiceAudio() noexcept = default;
  ~VoiceAudio() noexcept;
  VoiceAudio(const VoiceAudio&) = delete;
  VoiceAudio& operator=(const VoiceAudio&) = delete;

  /// 打开声卡和算法；音量范围为 0～100，超出上限按 100 处理。
  bool Open(std::uint8_t volume = 60U);
  /// 对话线程按顺序消费事件；即使暂时无事件，也要持续 Poll 以处理采集帧。
  bool Poll(AudioEvent* event, std::chrono::milliseconds timeout);
  /// 开始等候人声；追问使用较长的人声确认窗口，降低回复尾音触发的概率。
  bool Listen(bool follow_up);
  /// 提交一帧回复音频。start 仅允许 sequence=0；只有末帧可不足 20 ms。
  bool Play(std::uint32_t generation, const std::uint8_t* pcm, std::size_t bytes,
            std::uint32_t sequence, bool start, bool end);
  /// 立即停止扬声器，保留持续采集。
  void StopPlayback();
  /// 取消上行录音及待取事件；不会停止当前回复的播放。
  void CancelInput();
  void SetVolume(std::uint8_t volume);
  bool healthy() const;
  std::string last_error() const;
  /// 等待音频线程退出后释放声卡和算法，允许重复调用。
  void Close() noexcept;

 private:
  struct Impl;
  Impl* impl_{nullptr};
};

}  // namespace boompi::audio
