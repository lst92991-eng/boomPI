#pragma once

/**
 * @file voice_audio.h
 * @brief application 使用的唯一语音音频边界。
 *
 * 板级采样格式、VAD、500 ms pre-roll、AEC 打断探针和播放缓冲全部留在实现内。
 * application 只按顺序消费语义事件和固定 20 ms PCM，不接触声学标定参数。
 */

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>

#include "boompi/audio/board_voice_profile.h"

namespace boompi::audio {

enum class AudioEventKind : std::uint8_t {
  Wake,
  SpeechStart,
  Pcm,
  SpeechEnd,
  Barge,
  PlaybackDone,
  Fault,
};

struct AudioEvent final {
  AudioEventKind kind{AudioEventKind::Fault};
  std::uint32_t generation{0U};
  std::array<std::int16_t, kVoiceFrameSamples> pcm{};
  std::uint64_t sequence{0U};
  std::uint64_t timestamp_us{0U};
  bool end{false};
};

class VoiceAudio final {
 public:
  VoiceAudio() noexcept = default;
  ~VoiceAudio() noexcept;
  VoiceAudio(const VoiceAudio&) = delete;
  VoiceAudio& operator=(const VoiceAudio&) = delete;

  bool Open(std::uint8_t volume = 60U);
  bool Poll(AudioEvent* event, std::chrono::milliseconds timeout);
  bool Listen(bool follow_up);
  bool Play(std::uint32_t generation, const std::uint8_t* pcm,
            std::size_t bytes, std::uint32_t sequence, bool start, bool end);
  void StopPlayback();
  void CancelInput();
  void SetVolume(std::uint8_t volume);
  bool healthy() const;
  std::string last_error() const;
  void Close() noexcept;

 private:
  struct Impl;
  Impl* impl_{nullptr};
};

}  // namespace boompi::audio
