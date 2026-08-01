/// @file 音频编排：有界 TTS 队列、唯一播放线程和跨线程时序。
#include "boompi/audio/audio_engine.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <new>
#include <thread>

#include "audio_backend.h"

namespace boompi::audio { namespace {
constexpr std::size_t kTts24FrameSamples = 480U;
constexpr std::size_t kTtsSlots = 75U;       // 75 × 20 ms = 1.5 s，下行抖动上限。
constexpr std::size_t kInitialTtsSlots = 9U; // 首播累积 180 ms；EOS 短回复例外。
}

struct AudioEngine::Impl final {
  struct TtsSlot final { std::array<std::int16_t, kTts24FrameSamples> pcm{}; std::size_t used{0U}; };

  // backend 本身没有线程；本类规定主线程使用 capture 侧，playback_thread 使用播放侧。
  AudioEngineConfig config{};
  platform::rv1106::AudioBackend backend{};
  std::thread playback_thread{}; mutable std::mutex mutex{}, error_mutex{};
  std::condition_variable condition{}; std::array<char, 192U> error{};
  std::array<TtsSlot, kTtsSlots> tts{};
  std::size_t tts_head{0U}, tts_tail{0U}, tts_count{0U}, queued_samples{0U};
  std::uint64_t capture_sequence{0U}, next_tts_sequence{0U};
  bool open{false}, stop{false}, active{false}, ending{false}, playback_started{false};
  bool drop{false}, sequence_set{false};
  std::atomic<bool> duck{false}, is_playing{false}, is_done{true}, playback_failed{false};

  void SetError(const char* text) noexcept {
    std::lock_guard<std::mutex> lock(error_mutex); std::snprintf(error.data(), error.size(), "%s", text);
  }
  void ClearQueue() noexcept {
    for (auto& slot : tts) { slot.pcm.fill(0); slot.used = 0U; }
    tts_head = tts_tail = tts_count = queued_samples = 0U;
  }

  /// 播放线程只负责下行渲染；AEC reference 由 Codec Mode1 与双麦在同一 capture period 回采。
  bool Render(const TtsSlot& slot) noexcept {
    const float gain = config.playback_gain * (duck.load(std::memory_order_relaxed) ? config.duck_gain : 1.0F);
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
    duck.store(false, std::memory_order_release);
    playback_failed.store(failed, std::memory_order_release);
    is_playing.store(false, std::memory_order_release); is_done.store(true, std::memory_order_release);
    condition.notify_all();
  }

  /// 此线程只消费 TTS 并写 ALSA；绝不访问 capture、会话状态或网络。
  void PlaybackLoop() noexcept {
    for (;;) {
      TtsSlot slot{}; bool have_slot = false, finish = false, must_drop = false;
      {
        std::unique_lock<std::mutex> lock(mutex);
        condition.wait(lock, [this] {
          const bool initial_ready = queued_samples >= kInitialTtsSlots * kTts24FrameSamples;
          return stop || drop || (active && ((tts_count != 0U &&
              (tts[tts_head].used == kTts24FrameSamples || ending) &&
              (playback_started || initial_ready || ending)) || (ending && tts_count == 0U)));
        });
        if (stop) break;
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
  if (config.playback_gain < 0.0F || config.duck_gain < 0.0F || config.duck_gain > 1.0F) {
    impl_->SetError("invalid playback gain configuration"); return false;
  }
  try {
    impl_->config = config;
  } catch (...) { impl_->SetError("audio configuration allocation failed"); return false; }
  if (!impl_->backend.Open(config)) return false;
  impl_->stop = false; impl_->open = true;
  try { impl_->playback_thread = std::thread(&Impl::PlaybackLoop, impl_); }
  catch (...) { impl_->SetError("playback thread creation failed"); Close(); return false; }
  return true;
}

bool AudioEngine::Capture(CaptureFrame* const frame) noexcept {
  // 四通道 period 内已经同时包含双麦与双硬件 reference，不再跨线程估算播放时轴。
  if (impl_ == nullptr || !impl_->open || frame == nullptr) return false;
  bool discontinuity = false;
  if (!impl_->backend.ReadCapture20ms(&discontinuity)) return false;
  if (!impl_->backend.ProcessCapture20ms(discontinuity, frame)) return false;
  frame->sequence = impl_->capture_sequence++; return true;
}

bool AudioEngine::ResetListener() noexcept { return impl_ != nullptr && impl_->open && impl_->backend.ResetListener(); }

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
  // active=false 保证播放线程没有使用重采样器，主线程此刻可以安全复位其相位。
  if (!impl_->backend.PreparePlayback()) return false;
  impl_->sequence_set = impl_->ending = impl_->drop = impl_->playback_started = false; impl_->active = true;
  impl_->duck.store(false, std::memory_order_release);
  impl_->playback_failed.store(false, std::memory_order_release);
  impl_->is_playing.store(true, std::memory_order_release); impl_->is_done.store(false, std::memory_order_release);
  return true;
}

bool AudioEngine::EndPlayback() noexcept {
  if (impl_ == nullptr || !impl_->open) return false;
  std::lock_guard<std::mutex> lock(impl_->mutex);
  if (!impl_->active || impl_->ending) return false;
  impl_->ending = true; impl_->condition.notify_one(); return true;
}

void AudioEngine::Duck(bool enabled) noexcept { if (impl_ != nullptr) impl_->duck.store(enabled, std::memory_order_release); }
void AudioEngine::DropPlayback() noexcept {
  if (impl_ == nullptr || !impl_->open) return;
  std::lock_guard<std::mutex> lock(impl_->mutex);
  if (impl_->active) {
    impl_->drop = true; impl_->ClearQueue();
    // 持有状态锁发出中断，保证播放线程不会在 drop 之前先完成并重置 PCM。
    impl_->backend.InterruptPlayback(); impl_->condition.notify_one();
  }
}
bool AudioEngine::playing() const noexcept { return impl_ != nullptr && impl_->is_playing.load(std::memory_order_acquire); }
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
    { std::lock_guard<std::mutex> lock(impl_->mutex); impl_->stop = true; }
    // snd_pcm_drop 用于打断可能阻塞的 write/drain；join 完成前不得销毁 backend。
    impl_->condition.notify_all(); impl_->backend.InterruptPlayback();
    if (impl_->playback_thread.joinable()) impl_->playback_thread.join();
  }
  impl_->backend.Close();
  impl_->open = impl_->stop = impl_->active = impl_->ending = impl_->drop = impl_->playback_started = false;
  impl_->sequence_set = false; impl_->capture_sequence = 0U;
  impl_->ClearQueue();
  impl_->playback_failed.store(false, std::memory_order_release);
  impl_->duck.store(false, std::memory_order_release);
  impl_->is_playing.store(false, std::memory_order_release); impl_->is_done.store(true, std::memory_order_release);
}

}  // namespace boompi::audio
