#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "boompi/audio/audio_engine.h"

namespace boompi::test::audio_backend {

struct RenderCall final {
  std::vector<std::int16_t> pcm;
  float gain{0.0F};
};

enum class PlaybackBlock { kNone, kRender, kDrain };
void BlockPlayback(PlaybackBlock stage) noexcept;
void FailPlaybackPreparation() noexcept;
bool WaitForPlaybackBlocked(std::chrono::milliseconds timeout) noexcept;
bool PlaybackOwnerOrderIsValid() noexcept;

void Reset() noexcept;
void InjectPlaybackXruns(std::uint64_t count) noexcept;
std::uint64_t PlaybackXrunsSnapshot() noexcept;
void PushCapture(const audio::CaptureFrame& frame) noexcept;
bool WaitForCaptureReads(std::size_t count, std::chrono::milliseconds timeout) noexcept;
bool WaitForProcessedFrames(std::size_t count, std::chrono::milliseconds timeout) noexcept;
bool WaitForRenderCalls(std::size_t count, std::chrono::milliseconds timeout) noexcept;
bool WaitForCaptureInterrupts(std::size_t count, std::chrono::milliseconds timeout) noexcept;
std::vector<RenderCall> RenderCallsSnapshot();

}  // namespace boompi::test::audio_backend

namespace boompi::platform::rv1106 {

using audio::AudioEngineConfig;
using audio::CaptureFrame;

// AudioEngine host harness backend. This header intentionally shadows the
// RV1106 backend only for the dedicated test target.
class AudioBackend final {
 public:
  AudioBackend() noexcept = default;
  ~AudioBackend() noexcept;
  AudioBackend(const AudioBackend&) = delete;
  AudioBackend& operator=(const AudioBackend&) = delete;

  bool Open(const AudioEngineConfig& config) noexcept;
  bool ReadCapture20ms(bool* discontinuity) noexcept;
  bool ProcessCapture20ms(bool discontinuity, CaptureFrame* frame) noexcept;
  bool ResetListener() noexcept;
  bool ArmPlayback() noexcept;
  bool PreparePlayback() noexcept;
  bool Render20ms(const std::int16_t* pcm24, std::size_t samples, float gain) noexcept;
  bool DrainPlayback() noexcept;
  void DropPlayback() noexcept;
  void InterruptCapture() noexcept;
  void InterruptPlayback() noexcept;
  std::uint64_t playback_xruns() const noexcept;
  std::string last_error() const;
  void Close() noexcept;

 private:
  struct Impl;
  Impl* impl_{nullptr};
};

}  // namespace boompi::platform::rv1106
