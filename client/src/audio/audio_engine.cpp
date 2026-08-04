/// @file 音频编排：有界 TTS 队列、唯一播放线程和跨线程时序。
#include "boompi/audio/audio_engine.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <new>
#include <pthread.h>
#include <sched.h>
#include <thread>

#include "audio_backend.h"

namespace boompi::audio { namespace {
constexpr std::size_t kTts24FrameSamples = 480U;
constexpr std::size_t kTtsSlots = 75U;       // 75 × 20 ms = 1.5 s，下行抖动上限。
constexpr std::size_t kInitialTtsSlots = 9U; // 首播累积 180 ms；EOS 短回复例外。
constexpr std::size_t kCaptureSlots = 4U;    // actor 最多落后 80 ms；再慢就显式断帧。
constexpr int kCapturePriority = 40;
constexpr int kPlaybackPriority = 30;

void SetRealtimePriority(const char* name, int priority) noexcept {
  static_cast<void>(pthread_setname_np(pthread_self(), name));
  sched_param parameters{};
  parameters.sched_priority = priority;
  const int result = pthread_setschedparam(
      pthread_self(), SCHED_FIFO, &parameters);
  if (result != 0) {
    std::fprintf(stderr,
                 "boompi-client: warning: %s realtime priority %d failed (%d)\n",
                 name, priority, result);
  }
}
}

struct AudioEngine::Impl final {
  struct TtsSlot final { std::array<std::int16_t, kTts24FrameSamples> pcm{}; std::size_t used{0U}; };
  enum class CaptureCommand : std::uint8_t {
    kNone, kResetListener, kPreparePlayback
  };

  // capture_thread 独占采集/3A；playback_thread 独占播放热路径。application 仅消费固定环。
  platform::rv1106::AudioBackend backend{};
  std::thread capture_thread{}, playback_thread{};
  mutable std::mutex mutex{}, capture_mutex{}, error_mutex{};
  std::condition_variable condition{}, capture_condition{};
  std::array<char, 192U> error{};
  std::array<CaptureFrame, kCaptureSlots> capture{};
  std::size_t capture_head{0U}, capture_count{0U};
  CaptureCommand capture_command{CaptureCommand::kNone};
  std::uint64_t command_requested{0U}, command_completed{0U};
  bool command_succeeded{false};
  std::array<TtsSlot, kTtsSlots> tts{};
  std::size_t tts_head{0U}, tts_tail{0U}, tts_count{0U}, queued_samples{0U};
  std::uint64_t capture_sequence{0U}, next_tts_sequence{0U};
  bool open{false}, capture_failed{false};
  bool active{false}, ending{false}, playback_started{false};
  bool drop{false}, sequence_set{false};
  std::atomic<bool> stop{false};
  std::atomic<float> playback_gain{1.0F}, playback_scale{1.0F};
  std::atomic<bool> is_done{true}, playback_failed{false};

  void SetError(const char* text) noexcept {
    std::lock_guard<std::mutex> lock(error_mutex); std::snprintf(error.data(), error.size(), "%s", text);
  }
  void ClearQueue() noexcept {
    for (auto& slot : tts) { slot.pcm.fill(0); slot.used = 0U; }
    tts_head = tts_tail = tts_count = queued_samples = 0U;
  }

  void ClearCaptureQueue() noexcept {
    for (auto& frame : capture) frame = {};
    capture_head = capture_count = 0U;
  }

  bool RunCaptureCommand(CaptureCommand command) noexcept {
    std::unique_lock<std::mutex> lock(capture_mutex);
    if (stop.load(std::memory_order_acquire) || capture_failed ||
        command_requested != command_completed) return false;
    capture_command = command;
    const std::uint64_t requested = ++command_requested;
    capture_condition.notify_all();
    capture_condition.wait(lock, [this, requested] {
      return stop.load(std::memory_order_acquire) || capture_failed ||
             command_completed >= requested;
    });
    return !stop.load(std::memory_order_acquire) && !capture_failed &&
           command_completed >= requested && command_succeeded;
  }

  bool ApplyCaptureCommand() noexcept {
    CaptureCommand command = CaptureCommand::kNone;
    std::uint64_t requested = 0U;
    {
      std::lock_guard<std::mutex> lock(capture_mutex);
      if (command_requested == command_completed) return true;
      command = capture_command;
      requested = command_requested;
    }
    const bool succeeded = command == CaptureCommand::kResetListener
                               ? backend.ResetListener()
                               : command == CaptureCommand::kPreparePlayback
                                     ? backend.PreparePlayback()
                                     : false;
    {
      std::lock_guard<std::mutex> lock(capture_mutex);
      if (command == CaptureCommand::kResetListener) ClearCaptureQueue();
      command_succeeded = succeeded;
      command_completed = requested;
      capture_command = CaptureCommand::kNone;
    }
    capture_condition.notify_all();
    return succeeded;
  }

  void CaptureLoop() noexcept {
    SetRealtimePriority("boompi-capture", kCapturePriority);
    for (;;) {
      if (!ApplyCaptureCommand()) {
        std::lock_guard<std::mutex> lock(capture_mutex);
        capture_failed = true;
        capture_condition.notify_all();
        break;
      }
      if (stop.load(std::memory_order_acquire)) break;
      CaptureFrame frame{};
      bool discontinuity = false;
      bool succeeded = backend.ReadCapture20ms(&discontinuity);
      if (succeeded && !stop.load(std::memory_order_acquire))
        succeeded = backend.ProcessCapture20ms(discontinuity, &frame);
      if (succeeded && !stop.load(std::memory_order_acquire)) {
        frame.sequence = capture_sequence++;
        std::lock_guard<std::mutex> queue_lock(capture_mutex);
        if (frame.discontinuity || capture_count == capture.size()) {
          const bool actor_overrun = !frame.discontinuity;
          ClearCaptureQueue();
          frame.discontinuity = true;
          frame.actor_overrun = actor_overrun;
        }
        const std::size_t tail =
            (capture_head + capture_count) % capture.size();
        capture[tail] = frame;
        ++capture_count;
      }
      if (stop.load(std::memory_order_acquire)) break;
      if (!succeeded) {
        std::lock_guard<std::mutex> lock(capture_mutex);
        capture_failed = true;
        capture_condition.notify_all();
        break;
      }
      capture_condition.notify_one();
    }
  }

  /// 播放线程只负责下行渲染；AEC reference 由 Codec Mode1 与双麦在同一 capture period 回采。
  bool Render(const TtsSlot& slot) noexcept {
    const float gain = playback_gain.load(std::memory_order_relaxed) *
                       playback_scale.load(std::memory_order_relaxed);
    return backend.Render20ms(slot.pcm.data(), slot.used, gain);
  }

  void FinishPlayback(bool discard, bool failed) noexcept {
    // drain/write 期间仍可能收到用户 drop；每次阻塞调用返回后必须重新观察取消。
    {
      std::lock_guard<std::mutex> lock(mutex);
      if (drop) { discard = true; failed = false; }
    }
    if (discard || failed) backend.DropPlayback(); else failed = !backend.DrainPlayback();
    std::lock_guard<std::mutex> lock(mutex);
    // DropPlayback 持有同一把锁先中断 PCM；因此这里可安全完成 prepare。
    if (drop && !discard) { discard = true; failed = false; backend.DropPlayback(); }
    active = ending = drop = playback_started = false; ClearQueue();
    playback_scale.store(1.0F, std::memory_order_release);
    playback_failed.store(failed, std::memory_order_release);
    is_done.store(true, std::memory_order_release);
    condition.notify_all();
  }

  /// 此线程只消费 TTS 并写 ALSA；绝不访问 capture、会话状态或网络。
  void PlaybackLoop() noexcept {
    SetRealtimePriority("boompi-playback", kPlaybackPriority);
    for (;;) {
      TtsSlot slot{}; bool have_slot = false, finish = false, must_drop = false;
      {
        std::unique_lock<std::mutex> lock(mutex);
        condition.wait(lock, [this] {
          const bool initial_ready = queued_samples >= kInitialTtsSlots * kTts24FrameSamples;
          return stop.load(std::memory_order_acquire) || drop || (active && ((tts_count != 0U &&
              (tts[tts_head].used == kTts24FrameSamples || ending) &&
              (playback_started || initial_ready || ending)) || (ending && tts_count == 0U)));
        });
        if (stop.load(std::memory_order_acquire)) break;
        must_drop = drop;
        if (!must_drop && tts_count != 0U) {
          playback_started = true;
          slot = tts[tts_head]; queued_samples -= slot.used; tts[tts_head] = {};
          tts_head = (tts_head + 1U) % tts.size(); --tts_count; have_slot = true;
        }
        finish = must_drop || (ending && tts_count == 0U);
      }
      bool failed = have_slot && !Render(slot);
      if (finish || failed) FinishPlayback(must_drop, failed);
    }
  }

};

AudioEngine::~AudioEngine() noexcept { Close(); delete impl_; }

bool AudioEngine::Open(const AudioEngineConfig& config) noexcept {
  if (impl_ == nullptr) impl_ = new (std::nothrow) Impl;
  if (impl_ == nullptr) return false;
  if (impl_->open) { impl_->SetError("audio is already open"); return false; }
  if (config.playback_gain < 0.0F) {
    impl_->SetError("invalid playback gain configuration"); return false;
  }
  if (!impl_->backend.Open(config)) return false;
  impl_->playback_gain.store(config.playback_gain, std::memory_order_release);
  impl_->stop.store(false, std::memory_order_release);
  impl_->capture_failed = false;
  impl_->capture_command = Impl::CaptureCommand::kNone;
  impl_->command_requested = impl_->command_completed = 0U;
  impl_->command_succeeded = false;
  impl_->capture_sequence = 0U;
  impl_->ClearCaptureQueue();
  impl_->open = true;
  try {
    impl_->capture_thread = std::thread(&Impl::CaptureLoop, impl_);
    impl_->playback_thread = std::thread(&Impl::PlaybackLoop, impl_);
  } catch (...) {
    impl_->SetError("audio realtime thread creation failed");
    Close();
    return false;
  }
  return true;
}

bool AudioEngine::Capture(CaptureFrame* const frame) noexcept {
  // 高优先级采集线程持续排空 ALSA；application actor 只消费已处理的固定帧。
  if (impl_ == nullptr || !impl_->open || frame == nullptr) return false;
  std::unique_lock<std::mutex> lock(impl_->capture_mutex);
  impl_->capture_condition.wait(lock, [this] {
    return impl_->stop.load(std::memory_order_acquire) ||
           impl_->capture_failed || impl_->capture_count != 0U;
  });
  if (impl_->stop.load(std::memory_order_acquire) ||
      impl_->capture_failed) return false;
  *frame = impl_->capture[impl_->capture_head];
  impl_->capture[impl_->capture_head] = {};
  impl_->capture_head = (impl_->capture_head + 1U) % impl_->capture.size();
  --impl_->capture_count;
  return true;
}

bool AudioEngine::ResetListener() noexcept {
  if (impl_ == nullptr || !impl_->open ||
      impl_->stop.load(std::memory_order_acquire)) return false;
  return impl_->RunCaptureCommand(Impl::CaptureCommand::kResetListener);
}

bool AudioEngine::QueueTts24k(const std::uint8_t* const bytes, const std::size_t byte_count,
                        const std::uint64_t sequence) noexcept {
  if (impl_ == nullptr || !impl_->open || bytes == nullptr || byte_count == 0U || (byte_count & 1U) != 0U) return false;
  const std::size_t samples = byte_count / 2U;
  // WSS 事件已经回到 actor 线程，此处是 TTS ring 的唯一生产者。
  std::lock_guard<std::mutex> lock(impl_->mutex);
  if (!impl_->active || impl_->ending || samples > kTtsSlots * kTts24FrameSamples - impl_->queued_samples ||
      (impl_->sequence_set && sequence != impl_->next_tts_sequence)) {
    impl_->SetError("TTS queue is full, closed, or discontinuous"); return false;
  }
  std::size_t source = 0U;
  while (source < samples) {
    if (impl_->tts_count == 0U || impl_->tts[(impl_->tts_tail + kTtsSlots - 1U) % kTtsSlots].used == kTts24FrameSamples) {
      impl_->tts_tail = (impl_->tts_tail + 1U) % kTtsSlots; ++impl_->tts_count;
    }
    auto& slot = impl_->tts[(impl_->tts_tail + kTtsSlots - 1U) % kTtsSlots];
    const std::size_t copied = std::min(samples - source, kTts24FrameSamples - slot.used);
    for (std::size_t i = 0U; i < copied; ++i) {
      const std::size_t offset = 2U * (source + i);
      const std::uint16_t value = static_cast<std::uint16_t>(bytes[offset]) |
          (static_cast<std::uint16_t>(bytes[offset + 1U]) << 8U);
      slot.pcm[slot.used + i] = static_cast<std::int16_t>(value);
    }
    slot.used += copied; source += copied;
  }
  impl_->queued_samples += samples; impl_->sequence_set = true; impl_->next_tts_sequence = sequence + 1U;
  impl_->condition.notify_one(); return true;
}

bool AudioEngine::BeginPlayback() noexcept {
  if (impl_ == nullptr || !impl_->open) return false;
  std::lock_guard<std::mutex> lock(impl_->mutex);
  if (impl_->active) return false;
  impl_->ClearQueue();
  // capture 线程在帧边界执行 prepare，后端控制字段不跨线程读写。
  if (!impl_->RunCaptureCommand(Impl::CaptureCommand::kPreparePlayback))
    return false;
  impl_->sequence_set = impl_->ending = impl_->drop = impl_->playback_started = false; impl_->active = true;
  impl_->playback_scale.store(1.0F, std::memory_order_release);
  impl_->playback_failed.store(false, std::memory_order_release);
  impl_->is_done.store(false, std::memory_order_release);
  return true;
}

bool AudioEngine::EndPlayback() noexcept {
  if (impl_ == nullptr || !impl_->open) return false;
  std::lock_guard<std::mutex> lock(impl_->mutex);
  if (!impl_->active || impl_->ending) return false;
  impl_->ending = true; impl_->condition.notify_one(); return true;
}

void AudioEngine::SetPlaybackGain(const float gain) noexcept {
  if (impl_ != nullptr) impl_->playback_gain.store(std::max(0.0F, gain), std::memory_order_release);
}
void AudioEngine::SetPlaybackScale(const float scale) noexcept {
  if (impl_ != nullptr) impl_->playback_scale.store(std::clamp(scale, 0.0F, 1.0F), std::memory_order_release);
}
void AudioEngine::DropPlayback() noexcept {
  if (impl_ == nullptr || !impl_->open) return;
  std::lock_guard<std::mutex> lock(impl_->mutex);
  if (impl_->active) {
    impl_->drop = true; impl_->ClearQueue();
    // 持有状态锁发出中断，保证播放线程不会在 drop 之前先完成并重置 PCM。
    impl_->backend.InterruptPlayback(); impl_->condition.notify_one();
  }
}
bool AudioEngine::playback_done() const noexcept { return impl_ == nullptr || impl_->is_done.load(std::memory_order_acquire); }
bool AudioEngine::playback_failed() const noexcept { return impl_ != nullptr && impl_->playback_failed.load(std::memory_order_acquire); }

std::string AudioEngine::last_error() const {
  if (impl_ == nullptr) return "audio is not allocated";
  { std::lock_guard<std::mutex> lock(impl_->error_mutex); if (impl_->error[0] != '\0') return impl_->error.data(); }
  return impl_->backend.last_error();
}

void AudioEngine::Close() noexcept {
  if (impl_ == nullptr) return;
  if (impl_->open) {
    impl_->stop.store(true, std::memory_order_release);
    // 经典 lost-wakeup：若 waiter 在“谓词已检查、尚未登记到等待队列”的窗口内，
    // 锁外 notify 会被丢掉导致永久阻塞。短暂持锁建立同步边界后再通知，
    // 保证 waiter 要么已看到 stop=true，要么在 notify 之前尚未进入等待。
    { std::lock_guard<std::mutex> lock(impl_->capture_mutex); }
    impl_->capture_condition.notify_all();
    { std::lock_guard<std::mutex> lock(impl_->mutex); }
    impl_->condition.notify_all();
    impl_->backend.InterruptPlayback();
    if (impl_->capture_thread.joinable()) impl_->capture_thread.join();
    if (impl_->playback_thread.joinable()) impl_->playback_thread.join();
  }
  impl_->backend.Close();
  impl_->open = impl_->active = impl_->ending = impl_->drop = impl_->playback_started = false;
  impl_->stop.store(false, std::memory_order_release);
  impl_->capture_failed = false;
  impl_->capture_command = Impl::CaptureCommand::kNone;
  impl_->command_requested = impl_->command_completed = 0U;
  impl_->command_succeeded = false;
  impl_->sequence_set = false; impl_->capture_sequence = 0U;
  impl_->ClearCaptureQueue();
  impl_->ClearQueue();
  impl_->playback_failed.store(false, std::memory_order_release);
  impl_->playback_scale.store(1.0F, std::memory_order_release);
  impl_->is_done.store(true, std::memory_order_release);
}

}  // namespace boompi::audio
