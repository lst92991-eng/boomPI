#ifndef BOOMPI_VOICE_AUDIO_ENGINE_H_
#define BOOMPI_VOICE_AUDIO_ENGINE_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace boompi::voice {

struct AudioEngineConfig final {
  std::string capture_pcm, playback_pcm;
  std::string snowboy_resource, snowboy_model;
  std::string snowboy_sensitivity{"0.5"};
  float snowboy_gain{1.0F}, playback_gain{1.8F}, duck_gain{0.12F};
  std::int8_t left_polarity{1}, right_polarity{1};
  std::int32_t vad_mode{3};
  std::uint16_t vad_start_ms{120U}, vad_end_ms{700U};
};

struct CaptureFrame final {
  static constexpr std::size_t kSamples = 320U;
  std::array<std::int16_t, kSamples> pcm{};
  std::uint64_t sequence{0U};
  bool wake{false}, vad_now{false}, vad_started{false}, vad_ended{false};
  bool discontinuity{false}, echo_context{false}, near_voice{false};
};

// The controller thread owns Open, Capture, and the control methods. Capture
// blocks for one 20 ms ALSA period and allocates nothing. The engine owns one
// playback thread; Close signals and joins it before releasing ALSA handles.
class AudioEngine final {
 public:
  AudioEngine() noexcept = default;
  ~AudioEngine() noexcept;
  AudioEngine(const AudioEngine&) = delete;
  AudioEngine& operator=(const AudioEngine&) = delete;

  bool Open(const AudioEngineConfig& config) noexcept;
  bool Capture(CaptureFrame* frame) noexcept;
  bool ResetListener() noexcept;
  bool QueueTts24k(const std::uint8_t* pcm_bytes, std::size_t byte_count, std::uint64_t sequence) noexcept;
  bool BeginPlayback() noexcept;
  bool EndPlayback() noexcept;
  void Duck(bool enabled) noexcept;
  void DropPlayback() noexcept;
  bool playing() const noexcept;
  bool playback_done() const noexcept;
  std::string last_error() const;
  void Close() noexcept;

 private:
  struct Impl;
  Impl* impl_{nullptr};
};

}  // namespace boompi::voice

#endif  // BOOMPI_VOICE_AUDIO_ENGINE_H_
