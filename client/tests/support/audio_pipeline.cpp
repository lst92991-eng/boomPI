/**
 * @file audio_pipeline.cpp
 * @brief 用条件变量和可控故障复现板端 I/O 时序，驱动真实音频引擎回归。
 *
 * 测试主线程只注入事实或读取快照；引擎的采集/播放线程仍按生产路径运行。
 * 所有共享状态由同一 mutex 保护，pending 帧只由采集线程在 Read→Process 间持有。
 */
#include "audio_pipeline.h"

#include <condition_variable>
#include <deque>
#include <mutex>
#include <new>
#include <thread>
#include <utility>

namespace boompi::test::audio_pipeline {
namespace {

/** @brief 测试与两个引擎线程的交接区；计数和线程 ID 用于验证顺序，不驱动产品状态。 */
struct SharedState final {
  std::mutex mutex;
  std::condition_variable condition;
  std::deque<audio::CaptureFrame> capture_frames;
  std::vector<RenderCall> render_calls;
  std::size_t capture_reads{0U};
  std::size_t processed_frames{0U};
  std::size_t capture_interrupts{0U};
  bool open{false};
  bool capture_interrupted{false};
  PlaybackBlock playback_block{PlaybackBlock::None};
  bool playback_blocked{false}, playback_interrupted{false};
  bool owner_order_valid{true}, playback_prepared{false};
  bool fail_playback_preparation{false};
  std::thread::id capture_thread{}, playback_thread{};
  std::size_t armed_sessions{0U}, prepared_sessions{0U};
};

/** @brief 每个测试进程共享一份脚本状态，场景开始前调用 Reset。 */
SharedState& State() {
  static SharedState state;
  return state;
}

/** @brief 在共享锁下等待谓词，条件变量释放锁期间允许产品线程推进。 */
template <typename Predicate>
bool WaitUntil(Predicate predicate, std::chrono::milliseconds timeout) noexcept {
  auto& state = State();
  std::unique_lock<std::mutex> lock(state.mutex);
  return state.condition.wait_for(lock, timeout, predicate);
}

/**
 * @brief 播放线程的可控阻塞点；调用方必须已经持有共享锁。
 * 先核对 Prepare/线程身份，再等待 Close 或 Interrupt；假 I/O 也有一秒退出上限。
 */
bool WaitInPlayback(PlaybackBlock stage, std::unique_lock<std::mutex>& lock) {
  auto& state = State();
  state.owner_order_valid &=
      state.playback_prepared && state.playback_thread == std::this_thread::get_id();
  if (state.playback_block == stage) {
    state.playback_blocked = true;
    state.condition.notify_all();
    // 测试本身也有截止时间：若产品漏掉 interrupt，报告失败而不是挂死测试进程。
    if (!state.condition.wait_for(lock, std::chrono::seconds(1), [&state] {
          return !state.open || state.playback_interrupted;
        })) {
      return false;
    }
  }
  return !state.playback_interrupted;
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
  state.open = false;
  state.capture_interrupted = false;
  state.playback_block = PlaybackBlock::None;
  state.playback_blocked = state.playback_interrupted = false;
  state.owner_order_valid = true;
  state.playback_prepared = false;
  state.fail_playback_preparation = false;
  state.capture_thread = state.playback_thread = {};
  state.armed_sessions = state.prepared_sessions = 0U;
}

void BlockPlayback(PlaybackBlock stage) noexcept {
  auto& state = State();
  std::lock_guard<std::mutex> lock(state.mutex);
  state.playback_block = stage;
}

void FailPlaybackPreparation() noexcept {
  auto& state = State();
  std::lock_guard<std::mutex> lock(state.mutex);
  state.fail_playback_preparation = true;
}

bool WaitForPlaybackBlocked(std::chrono::milliseconds timeout) noexcept {
  return WaitUntil(
      [] {
        return State().playback_blocked;
      },
      timeout);
}

bool PlaybackOwnerOrderIsValid() noexcept {
  auto& state = State();
  std::lock_guard<std::mutex> lock(state.mutex);
  return state.owner_order_valid && state.prepared_sessions != 0U &&
         state.armed_sessions == state.prepared_sessions;
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
  return WaitUntil(
      [count] {
        return State().capture_reads >= count;
      },
      timeout);
}

bool WaitForProcessedFrames(const std::size_t count,
                            const std::chrono::milliseconds timeout) noexcept {
  return WaitUntil(
      [count] {
        return State().processed_frames >= count;
      },
      timeout);
}

bool WaitForRenderCalls(const std::size_t count,
                        const std::chrono::milliseconds timeout) noexcept {
  return WaitUntil(
      [count] {
        return State().render_calls.size() >= count;
      },
      timeout);
}

bool WaitForCaptureInterrupts(const std::size_t count,
                              const std::chrono::milliseconds timeout) noexcept {
  return WaitUntil(
      [count] {
        return State().capture_interrupts >= count;
      },
      timeout);
}

std::vector<RenderCall> RenderCallsSnapshot() {
  auto& state = State();
  std::lock_guard<std::mutex> lock(state.mutex);
  return state.render_calls;
}

}  // namespace boompi::test::audio_pipeline

namespace boompi::platform::rv1106 {

/// Read 保留完整检测帧，Process 再交出，维持真实后端的两阶段输入接口。
struct AudioPipeline::Impl final {
  CaptureFrame pending{};
  bool has_pending{false};
};

AudioPipeline::~AudioPipeline() noexcept {
  Close();
  delete impl_;
}

bool AudioPipeline::Open() noexcept {
  if (impl_ == nullptr) {
    impl_ = new (std::nothrow) Impl;
  }
  if (impl_ == nullptr) {
    return false;
  }
  auto& state = test::audio_pipeline::State();
  {
    std::lock_guard<std::mutex> lock(state.mutex);
    state.open = true;
    state.capture_interrupted = false;
  }
  state.condition.notify_all();
  return true;
}

bool AudioPipeline::ReadCapture20ms(RawCaptureFrame* const raw) noexcept {
  if (impl_ == nullptr || raw == nullptr) {
    return false;
  }
  auto& state = test::audio_pipeline::State();
  std::unique_lock<std::mutex> lock(state.mutex);
  state.capture_thread = std::this_thread::get_id();
  ++state.capture_reads;
  state.condition.notify_all();
  state.condition.wait(lock, [&state] {
    return !state.open || state.capture_interrupted || !state.capture_frames.empty();
  });
  if (!state.open || state.capture_interrupted) {
    return false;
  }
  impl_->pending = state.capture_frames.front();
  // raw 这里只承载时刻/断点；PCM 和分类已由脚本准备，不能把本场景当作算法验证。
  state.capture_frames.pop_front();
  impl_->has_pending = true;
  raw->discontinuity = impl_->pending.discontinuity;
  raw->timestamp_us = impl_->pending.timestamp_us;
  return true;
}

bool AudioPipeline::ProcessCapture20ms(const RawCaptureFrame& raw,
                                       CaptureFrame* const frame) noexcept {
  if (impl_ == nullptr || frame == nullptr || !impl_->has_pending) {
    return false;
  }
  *frame = impl_->pending;
  frame->discontinuity = raw.discontinuity;
  impl_->has_pending = false;
  auto& state = test::audio_pipeline::State();
  {
    std::lock_guard<std::mutex> lock(state.mutex);
    ++state.processed_frames;
  }
  state.condition.notify_all();
  return true;
}

bool AudioPipeline::ResetListener() noexcept {
  return true;
}
bool AudioPipeline::ArmPlayback() noexcept {
  auto& state = test::audio_pipeline::State();
  std::lock_guard<std::mutex> lock(state.mutex);
  state.owner_order_valid &= state.capture_thread == std::this_thread::get_id();
  ++state.armed_sessions;
  return true;
}

bool AudioPipeline::PreparePlayback() noexcept {
  auto& state = test::audio_pipeline::State();
  std::lock_guard<std::mutex> lock(state.mutex);
  state.playback_thread = std::this_thread::get_id();
  state.owner_order_valid &= state.playback_thread != state.capture_thread &&
                             state.armed_sessions == state.prepared_sessions + 1U;
  ++state.prepared_sessions;
  state.playback_prepared = !state.fail_playback_preparation;
  state.playback_interrupted = false;
  return state.playback_prepared;
}

bool AudioPipeline::Render20ms(const std::int16_t* const pcm16, const std::size_t samples,
                               const float gain) noexcept {
  if (pcm16 == nullptr || samples == 0U) {
    return false;
  }
  test::audio_pipeline::RenderCall call;
  call.pcm.assign(pcm16, pcm16 + samples);
  call.gain = gain;
  auto& state = test::audio_pipeline::State();
  {
    std::unique_lock<std::mutex> lock(state.mutex);
    state.render_calls.push_back(std::move(call));
    if (!test::audio_pipeline::WaitInPlayback(test::audio_pipeline::PlaybackBlock::Render,
                                              lock)) {
      return false;
    }
  }
  state.condition.notify_all();
  // 每次假渲染同样推进 20 ms，避免瞬间耗尽 180 ms 初始缓存，制造并不存在的欠载。
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  return true;
}

bool AudioPipeline::DrainPlayback() noexcept {
  auto& state = test::audio_pipeline::State();
  std::unique_lock<std::mutex> lock(state.mutex);
  return test::audio_pipeline::WaitInPlayback(test::audio_pipeline::PlaybackBlock::Drain, lock);
}
void AudioPipeline::DropPlayback() noexcept {
  auto& state = test::audio_pipeline::State();
  std::lock_guard<std::mutex> lock(state.mutex);
  // 尚未渲染就取消时，允许播放线程直接 drop，不要求先 prepare。
  state.owner_order_valid &= std::this_thread::get_id() != state.capture_thread;
  state.playback_prepared = false;
}

void AudioPipeline::InterruptCapture() noexcept {
  auto& state = test::audio_pipeline::State();
  {
    std::lock_guard<std::mutex> lock(state.mutex);
    state.capture_interrupted = true;
    ++state.capture_interrupts;
  }
  state.condition.notify_all();
}

void AudioPipeline::InterruptPlayback() noexcept {
  auto& state = test::audio_pipeline::State();
  {
    std::lock_guard<std::mutex> lock(state.mutex);
    state.playback_interrupted = true;
  }
  state.condition.notify_all();
}
std::string AudioPipeline::LastError() const {
  return {};
}

void AudioPipeline::Close() noexcept {
  auto& state = test::audio_pipeline::State();
  {
    std::lock_guard<std::mutex> lock(state.mutex);
    state.open = false;
  }
  state.condition.notify_all();
}

}  // namespace boompi::platform::rv1106
