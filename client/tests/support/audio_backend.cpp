#include "audio_backend.h"

#include <condition_variable>
#include <deque>
#include <mutex>
#include <new>
#include <thread>
#include <utility>

namespace boompi::test::audio_backend { namespace {

struct SharedState final {
  std::mutex mutex;
  std::condition_variable condition;
  std::deque<audio::CaptureFrame> capture_frames;
  std::vector<RenderCall> render_calls;
  std::size_t capture_reads{0U};
  std::size_t processed_frames{0U};
  std::size_t capture_interrupts{0U};
  std::uint64_t playback_xruns{0U};
  bool open{false};
  bool capture_interrupted{false};
};

SharedState& State() {
  static SharedState state;
  return state;
}

template <typename Predicate>
bool WaitUntil(Predicate predicate, std::chrono::milliseconds timeout) noexcept {
  auto& state = State();
  std::unique_lock<std::mutex> lock(state.mutex);
  return state.condition.wait_for(lock, timeout, predicate);
}

}  // namespace

void Reset() noexcept {
  auto& state = State();
  std::lock_guard<std::mutex> lock(state.mutex);
  state.capture_frames.clear();
  state.render_calls.clear();
  state.capture_reads = 0U;
  state.processed_frames = 0U;
  state.capture_interrupts = 0U;
  state.playback_xruns = 0U;
  state.open = false;
  state.capture_interrupted = false;
}

void InjectPlaybackXruns(const std::uint64_t count) noexcept {
  auto& state = State();
  std::lock_guard<std::mutex> lock(state.mutex);
  state.playback_xruns += count;
}

std::uint64_t PlaybackXrunsSnapshot() noexcept {
  auto& state = State();
  std::lock_guard<std::mutex> lock(state.mutex);
  return state.playback_xruns;
}

void PushCapture(const audio::CaptureFrame& frame) noexcept {
  auto& state = State();
  {
    std::lock_guard<std::mutex> lock(state.mutex);
    state.capture_frames.push_back(frame);
  }
  state.condition.notify_all();
}

bool WaitForCaptureReads(const std::size_t count,
                         const std::chrono::milliseconds timeout) noexcept {
  return WaitUntil([count] { return State().capture_reads >= count; }, timeout);
}

bool WaitForProcessedFrames(const std::size_t count,
                            const std::chrono::milliseconds timeout) noexcept {
  return WaitUntil([count] { return State().processed_frames >= count; }, timeout);
}

bool WaitForRenderCalls(const std::size_t count,
                        const std::chrono::milliseconds timeout) noexcept {
  return WaitUntil([count] { return State().render_calls.size() >= count; },
                   timeout);
}

bool WaitForCaptureInterrupts(const std::size_t count,
                              const std::chrono::milliseconds timeout) noexcept {
  return WaitUntil([count] { return State().capture_interrupts >= count; },
                   timeout);
}

std::vector<RenderCall> RenderCallsSnapshot() {
  auto& state = State();
  std::lock_guard<std::mutex> lock(state.mutex);
  return state.render_calls;
}

}  // namespace boompi::test::audio_backend

namespace boompi::platform::rv1106 {

struct AudioBackend::Impl final {
  CaptureFrame pending{};
  bool has_pending{false};
};

AudioBackend::~AudioBackend() noexcept {
  Close();
  delete impl_;
}

bool AudioBackend::Open(const AudioEngineConfig&) noexcept {
  if (impl_ == nullptr) impl_ = new (std::nothrow) Impl;
  if (impl_ == nullptr) return false;
  auto& state = test::audio_backend::State();
  {
    std::lock_guard<std::mutex> lock(state.mutex);
    state.open = true;
    state.capture_interrupted = false;
  }
  state.condition.notify_all();
  return true;
}

bool AudioBackend::ReadCapture20ms(bool* const discontinuity) noexcept {
  if (impl_ == nullptr || discontinuity == nullptr) return false;
  auto& state = test::audio_backend::State();
  std::unique_lock<std::mutex> lock(state.mutex);
  ++state.capture_reads;
  state.condition.notify_all();
  state.condition.wait(lock, [&state] {
    return !state.open || state.capture_interrupted ||
           !state.capture_frames.empty();
  });
  if (!state.open || state.capture_interrupted) return false;
  impl_->pending = state.capture_frames.front();
  state.capture_frames.pop_front();
  impl_->has_pending = true;
  *discontinuity = impl_->pending.discontinuity;
  return true;
}

bool AudioBackend::ProcessCapture20ms(const bool discontinuity,
                                      CaptureFrame* const frame) noexcept {
  if (impl_ == nullptr || frame == nullptr || !impl_->has_pending) return false;
  *frame = impl_->pending;
  frame->discontinuity = discontinuity;
  impl_->has_pending = false;
  auto& state = test::audio_backend::State();
  {
    std::lock_guard<std::mutex> lock(state.mutex);
    ++state.processed_frames;
  }
  state.condition.notify_all();
  return true;
}

bool AudioBackend::ResetListener() noexcept { return true; }
bool AudioBackend::PreparePlayback() noexcept { return true; }

bool AudioBackend::Render20ms(const std::int16_t* const pcm24,
                              const std::size_t samples,
                              const float gain) noexcept {
  if (pcm24 == nullptr || samples == 0U) return false;
  test::audio_backend::RenderCall call;
  call.pcm.assign(pcm24, pcm24 + samples);
  call.gain = gain;
  auto& state = test::audio_backend::State();
  {
    std::lock_guard<std::mutex> lock(state.mutex);
    state.render_calls.push_back(std::move(call));
  }
  state.condition.notify_all();
  // The board backend advances one 20 ms ALSA period per call. Giving the
  // fake backend the same media clock prevents it from draining 180 ms of
  // queued audio instantaneously and turning scheduler jitter into underruns.
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  return true;
}

bool AudioBackend::DrainPlayback() noexcept { return true; }
void AudioBackend::DropPlayback() noexcept {}

void AudioBackend::InterruptCapture() noexcept {
  auto& state = test::audio_backend::State();
  {
    std::lock_guard<std::mutex> lock(state.mutex);
    state.capture_interrupted = true;
    ++state.capture_interrupts;
  }
  state.condition.notify_all();
}

void AudioBackend::InterruptPlayback() noexcept {}
std::uint64_t AudioBackend::playback_xruns() const noexcept {
  return test::audio_backend::PlaybackXrunsSnapshot();
}
std::string AudioBackend::last_error() const { return {}; }

void AudioBackend::Close() noexcept {
  auto& state = test::audio_backend::State();
  {
    std::lock_guard<std::mutex> lock(state.mutex);
    state.open = false;
  }
  state.condition.notify_all();
}

}  // namespace boompi::platform::rv1106
