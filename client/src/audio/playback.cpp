#include "boompi/audio/playback.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <thread>

#include "alsa_audio.h"
#include "audio_convert.h"
#include "audio_thread.h"
#include "board_voice_profile.h"
#include "frame_queue.h"

namespace boompi::playback {
namespace {
constexpr std::size_t kSlots = audio::VoiceFrameContract::FramesForMs(1500U);
constexpr std::size_t kInitialSlots = audio::VoiceFrameContract::FramesForMs(180U);
constexpr std::size_t kRebufferSlots = audio::VoiceFrameContract::FramesForMs(40U);
constexpr auto kRebufferGrace = std::chrono::milliseconds(30);
constexpr auto kCancelTimeout = std::chrono::milliseconds(60);
struct Slot {
  std::array<std::int16_t, audio::kTtsFrameSamples> pcm{};
  std::size_t used{0U};
};
std::mutex mutex, error_mutex;
std::condition_variable condition;
std::thread thread;
std::atomic<bool> stop{false};
bool opened{false}, ending{false}, started{false}, rebuffering{false}, canceled{false};
Status current;
audio::FrameQueue<Slot, kSlots> queue;
audio::StereoPlaybackFrame stereo;
std::atomic<float> volume_gain{0.6F}, scale{1.0F};
std::array<char, 192U> failure{};
// 输入线程只采集这份原子快照，不等待播放队列锁。
std::atomic<bool> render_started{false}, output_audible{false};
std::atomic<End> observed_end{End::None};

bool Fail(const char* reason) {
  std::lock_guard<std::mutex> lock(error_mutex);
  std::snprintf(failure.data(), failure.size(), "%s", reason);
  return false;
}

bool Ready() {
  if (current.state != State::Playing) {
    return false;
  }
  if (ending) {
    return true;  // 短回答不需要等到180ms，空队列进入尾播。
  }
  if (queue.Size() == 0U || queue[0].used != audio::kTtsFrameSamples) {
    return false;
  }
  const std::size_t full_slots =
      queue.Size() - (queue[queue.Size() - 1U].used != audio::kTtsFrameSamples ? 1U : 0U);
  return !started ? full_slots >= kInitialSlots : !rebuffering || full_slots >= kRebufferSlots;
}

bool WriteToSpeaker() {
  const float gain = volume_gain.load() * scale.load();
  const long peak = audio_convert::peak(stereo);
  if (peak != 0) {
    render_started.store(true);
    output_audible.store(gain > 0.0F);
  }
  audio_convert::apply_volume(&stereo, gain, peak);
  return alsa_audio::write(stereo.pcm.data(), stereo.frames);
}

bool InterruptedWithoutError() {
  // begin已清本轮设备错误；真实故障即使随后收到cancel也不能被改报为正常取消。
  return alsa_audio::playback_interrupted() && alsa_audio::playback_error().empty();
}

void Finish(bool discard, bool failed) {
  {
    std::lock_guard<std::mutex> lock(mutex);
    if (canceled || stop.load()) {
      discard = true;
    }
  }
  if (!discard && !failed) {
    // 网络END之后先取尽滤波器有效尾音，最后才等待ALSA内核缓冲播放完毕。
    do {
      if (!audio_convert::playback(nullptr, 0U, &stereo)) {
        Fail("TTS resampler tail failed");
        failed = true;
        break;
      }
      if (!WriteToSpeaker()) {
        discard = InterruptedWithoutError();
        failed = !discard;
        break;
      }
    } while (stereo.frames != 0U);
    if (!failed) {
      if (!discard && !alsa_audio::drain()) {
        discard = InterruptedWithoutError();
        failed = !discard;
      }
    }
  }
  if (discard || failed) {
    alsa_audio::drop();
  }
  std::lock_guard<std::mutex> lock(mutex);
  // cancel允许在write、滤波尾音write或drain期间发生；新一轮只能等收尾完成后开始。
  if (canceled || stop.load()) {
    alsa_audio::drop();
    discard = true;
  }
  failed = failed || !alsa_audio::playback_error().empty();
  render_started.store(false);
  output_audible.store(false);
  observed_end.store(discard ? End::Interrupted : End::Natural);
  current.state = failed ? State::Failed : discard ? State::Idle : State::Drained;
  // current.generation同时保留退休水位，取消不会把旧代身份清成可复用。
  ending = started = rebuffering = canceled = false;
  queue.Clear();
  scale.store(1.0F);
  condition.notify_all();
}

void PlaybackTask() {
  audio::SetAudioThreadPriority("boompi-playback", 30);
  while (true) {
    std::unique_lock<std::mutex> lock(mutex);
    if (current.state == State::Playing && started && queue.Size() == 0U && !ending &&
        !canceled && !rebuffering && !condition.wait_for(lock, kRebufferGrace, [] {
          return stop.load() || canceled || ending || queue.Size() != 0U;
        })) {
      rebuffering = true;
    }
    condition.wait(lock, [] {
      return stop.load() || canceled || Ready();
    });
    if (stop.load()) {
      break;
    }
    if (canceled) {
      lock.unlock();
      Finish(true, false);
      continue;
    }
    // 准备期间持锁，cancel不会在此后被prepare清掉；实际write不持队列锁。
    if (!started && (!alsa_audio::prepare_playback() || !audio_convert::reset_playback())) {
      Fail("playback preparation failed");
      lock.unlock();
      Finish(false, true);
      continue;
    }
    Slot frame;
    if (!queue.Pop(&frame)) {
      lock.unlock();
      Finish(false, false);
      continue;
    }
    started = true;
    rebuffering = false;
    const bool last = ending && queue.Size() == 0U;
    lock.unlock();

    // 取一包16kHz PCM → 直接转48kHz双声道 → 音量/限幅 → ALSA完整写入。
    const bool converted = audio_convert::playback(frame.pcm.data(), frame.used, &stereo);
    if (!converted) {
      Fail("TTS resampling failed");
    }
    const bool played = converted && WriteToSpeaker();
    if (last || !played) {
      Finish(false, !converted || (!played && !InterruptedWithoutError()));
    }
  }
}
}  // namespace

bool open(std::uint8_t volume) {
  if (opened) {
    return false;
  }
  {
    std::lock_guard<std::mutex> lock(error_mutex);
    failure.fill('\0');
  }
  if (!alsa_audio::open_playback(audio::kPlaybackPcm)) {
    return false;
  }
  if (!audio_convert::open_playback()) {
    close();
    return Fail("playback resampler initialization failed");
  }
  set_volume(volume);
  scale.store(1.0F);
  stop.store(false);
  current = {};
  opened = true;
  try {
    thread = std::thread(PlaybackTask);
  } catch (...) {
    close();
    return Fail("playback thread creation failed");
  }
  return true;
}

bool begin(std::uint32_t generation) {
  std::unique_lock<std::mutex> lock(mutex);
  if (!opened || current.state == State::Failed || generation == 0U ||
      generation <= current.generation || (current.state == State::Playing && !canceled)) {
    return false;
  }
  if (!condition.wait_for(lock, kCancelTimeout,
                          [] {
                            return stop.load() || current.state != State::Playing;
                          }) ||
      stop.load()) {
    return Fail("previous playback cancel timed out");
  }
  if (current.state == State::Failed) {
    return false;
  }
  render_started.store(false);
  output_audible.store(false);
  observed_end.store(End::None);
  {
    std::lock_guard<std::mutex> error_lock(error_mutex);
    failure.fill('\0');
  }
  alsa_audio::clear_playback_error();
  current = {generation, State::Playing};
  ending = started = rebuffering = canceled = false;
  queue.Clear();
  scale.store(1.0F);
  return true;
}

WriteResult write(std::uint32_t generation, const std::uint8_t* bytes, std::size_t byte_count) {
  if (bytes == nullptr || byte_count == 0U || byte_count > audio::kTtsFrameSamples * 2U ||
      (byte_count & 1U) != 0U) {
    return WriteResult::InvalidArgument;
  }
  std::lock_guard<std::mutex> lock(mutex);
  if (!opened) {
    return WriteResult::NotOpen;
  }
  if (current.state != State::Playing || canceled) {
    return WriteResult::NotActive;
  }
  if (generation != current.generation) {
    return WriteResult::StaleGeneration;
  }
  if (ending ||
      (queue.Size() != 0U && queue[queue.Size() - 1U].used != audio::kTtsFrameSamples)) {
    return WriteResult::Ending;
  }
  if (queue.Size() == kSlots) {
    return WriteResult::Full;
  }
  Slot frame;
  frame.used = byte_count / 2U;
  for (std::size_t i = 0U; i < frame.used; ++i) {
    const std::uint16_t sample = static_cast<std::uint16_t>(bytes[2U * i]) |
                                 (static_cast<std::uint16_t>(bytes[2U * i + 1U]) << 8U);
    frame.pcm[i] = static_cast<std::int16_t>(sample);
  }
  static_cast<void>(queue.Push(frame));
  condition.notify_all();
  return WriteResult::Queued;
}

bool finish(std::uint32_t generation) {
  std::lock_guard<std::mutex> lock(mutex);
  if (!opened || current.state != State::Playing || generation != current.generation ||
      canceled || ending) {
    return false;
  }
  ending = true;
  condition.notify_all();
  return true;
}
void cancel() {
  std::lock_guard<std::mutex> lock(mutex);
  if (opened && current.state == State::Playing) {
    canceled = true;
    queue.Clear();
    render_started.store(false);
    output_audible.store(false);
    observed_end.store(End::Interrupted);
    alsa_audio::interrupt_playback();
    condition.notify_all();
  }
}
void set_volume(std::uint8_t volume) {
  volume_gain.store(static_cast<float>(std::min<std::uint8_t>(volume, 100U)) / 100.0F);
}
void set_scale(float value) {
  scale.store(std::isfinite(value) ? std::clamp(value, 0.0F, 1.0F) : 0.0F);
}
Status status() {
  std::lock_guard<std::mutex> lock(mutex);
  return current;
}
Observation observe() {
  Observation observation;
  observation.end = observed_end.exchange(End::None);
  observation.render_started = render_started.load();
  observation.output_audible = output_audible.load();
  return observation;
}
std::string error() {
  std::lock_guard<std::mutex> lock(error_mutex);
  return failure[0] != '\0' ? std::string(failure.data()) : alsa_audio::playback_error();
}
void close() {
  stop.store(true);
  {
    std::lock_guard<std::mutex> lock(mutex);
    condition.notify_all();
    alsa_audio::interrupt_playback();
  }
  if (thread.joinable()) {
    thread.join();
  }
  alsa_audio::close_playback();
  audio_convert::close_playback();
  std::lock_guard<std::mutex> lock(mutex);
  opened = false;
  current = {};
  queue.Clear();
  ending = started = rebuffering = canceled = false;
  render_started.store(false);
  output_audible.store(false);
  observed_end.store(End::Interrupted);
}
}  // namespace boompi::playback
