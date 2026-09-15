#include "audio_hardware.h"

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

#include "alsa_audio.h"

namespace boompi::test::audio_hardware {
namespace {
struct Hardware {
  std::mutex mutex;
  std::condition_variable changed;
  std::deque<audio::RawCaptureFrame> captures;
  std::vector<std::int16_t> written;
  std::vector<std::string> operations;
  std::size_t reads{0}, interrupts{0}, writes{0}, drains{0}, prepares{0};
  bool capture_open{false}, playback_open{false};
  bool capture_interrupted{false}, playback_interrupted{false}, prepared{false};
  bool blocked{false}, fail_prepare{false}, fail_capture_open{false}, owner_valid{true};
  PlaybackBlock block{PlaybackBlock::None};
  std::thread::id capture_thread{}, playback_thread{};
};
Hardware hardware;

template <typename Predicate>
bool wait(Predicate predicate, std::chrono::milliseconds timeout) {
  std::unique_lock<std::mutex> lock(hardware.mutex);
  return hardware.changed.wait_for(lock, timeout, predicate);
}

bool playback_wait(PlaybackBlock stage, std::unique_lock<std::mutex>& lock) {
  hardware.owner_valid &=
      hardware.prepared && hardware.playback_thread == std::this_thread::get_id();
  if (hardware.block == stage) {
    hardware.blocked = true;
    hardware.changed.notify_all();
    if (!hardware.changed.wait_for(lock, std::chrono::seconds(1), [] {
          return !hardware.playback_open || hardware.playback_interrupted ||
                 hardware.block == PlaybackBlock::None;
        })) {
      return false;
    }
  }
  return hardware.playback_open && !hardware.playback_interrupted;
}
}  // namespace

void reset() {
  std::lock_guard<std::mutex> lock(hardware.mutex);
  hardware.captures.clear();
  hardware.written.clear();
  hardware.operations.clear();
  hardware.reads = hardware.interrupts = hardware.writes = hardware.drains = hardware.prepares =
      0;
  hardware.capture_open = hardware.playback_open = hardware.prepared = false;
  hardware.capture_interrupted = hardware.playback_interrupted = hardware.blocked = false;
  hardware.fail_prepare = false;
  hardware.fail_capture_open = false;
  hardware.owner_valid = true;
  hardware.block = PlaybackBlock::None;
  hardware.capture_thread = hardware.playback_thread = {};
}
void push_capture(const audio::RawCaptureFrame& frame) {
  std::lock_guard<std::mutex> lock(hardware.mutex);
  hardware.captures.push_back(frame);
  hardware.changed.notify_all();
}
void block_playback(PlaybackBlock stage) {
  std::lock_guard<std::mutex> lock(hardware.mutex);
  hardware.block = stage;
  hardware.blocked = false;
  hardware.changed.notify_all();
}
void fail_playback_preparation(bool fail) {
  std::lock_guard<std::mutex> lock(hardware.mutex);
  hardware.fail_prepare = fail;
}
void fail_capture_open(bool fail) {
  std::lock_guard<std::mutex> lock(hardware.mutex);
  hardware.fail_capture_open = fail;
}
bool wait_for_capture_reads(std::size_t count, std::chrono::milliseconds timeout) {
  return wait(
      [count] {
        return hardware.reads >= count;
      },
      timeout);
}
bool wait_for_writes(std::size_t count, std::chrono::milliseconds timeout) {
  return wait(
      [count] {
        return hardware.writes >= count;
      },
      timeout);
}
bool wait_for_playback_blocked(std::chrono::milliseconds timeout) {
  return wait(
      [] {
        return hardware.blocked;
      },
      timeout);
}
std::size_t capture_reads() {
  std::lock_guard<std::mutex> lock(hardware.mutex);
  return hardware.reads;
}
std::size_t capture_interrupts() {
  std::lock_guard<std::mutex> lock(hardware.mutex);
  return hardware.interrupts;
}
std::size_t write_count() {
  std::lock_guard<std::mutex> lock(hardware.mutex);
  return hardware.writes;
}
std::size_t drain_count() {
  std::lock_guard<std::mutex> lock(hardware.mutex);
  return hardware.drains;
}
bool owner_order_valid() {
  std::lock_guard<std::mutex> lock(hardware.mutex);
  return hardware.owner_valid && hardware.prepares != 0 &&
         hardware.capture_thread != hardware.playback_thread;
}
std::vector<std::int16_t> written_samples() {
  std::lock_guard<std::mutex> lock(hardware.mutex);
  return hardware.written;
}
std::vector<std::string> operations() {
  std::lock_guard<std::mutex> lock(hardware.mutex);
  return hardware.operations;
}
}  // namespace boompi::test::audio_hardware

namespace boompi::alsa_audio {
namespace hw = test::audio_hardware;

bool open_capture(const std::string&) noexcept {
  std::lock_guard<std::mutex> lock(hw::hardware.mutex);
  hw::hardware.capture_open = !hw::hardware.fail_capture_open;
  hw::hardware.capture_interrupted = false;
  hw::hardware.operations.push_back("capture.open");
  return hw::hardware.capture_open;
}
bool open_playback(const std::string&) noexcept {
  std::lock_guard<std::mutex> lock(hw::hardware.mutex);
  hw::hardware.playback_open = true;
  hw::hardware.playback_interrupted = false;
  hw::hardware.operations.push_back("playback.open");
  return true;
}
bool read(std::int16_t* output, bool* discontinuity) noexcept {
  std::unique_lock<std::mutex> lock(hw::hardware.mutex);
  hw::hardware.capture_thread = std::this_thread::get_id();
  ++hw::hardware.reads;
  hw::hardware.changed.notify_all();
  hw::hardware.changed.wait(lock, [] {
    return !hw::hardware.capture_open || hw::hardware.capture_interrupted ||
           !hw::hardware.captures.empty();
  });
  if (!hw::hardware.capture_open || hw::hardware.capture_interrupted || output == nullptr ||
      discontinuity == nullptr) {
    return false;
  }
  const auto frame = hw::hardware.captures.front();
  hw::hardware.captures.pop_front();
  std::copy(frame.pcm.begin(), frame.pcm.end(), output);
  *discontinuity = frame.discontinuity;
  return true;
}
bool prepare_playback() noexcept {
  std::unique_lock<std::mutex> lock(hw::hardware.mutex);
  hw::hardware.playback_thread = std::this_thread::get_id();
  hw::hardware.owner_valid &= hw::hardware.playback_thread != hw::hardware.capture_thread;
  ++hw::hardware.prepares;
  hw::hardware.operations.push_back("playback.prepare");
  if (hw::hardware.block == hw::PlaybackBlock::Prepare) {
    hw::hardware.blocked = true;
    hw::hardware.changed.notify_all();
    if (!hw::hardware.changed.wait_for(lock, std::chrono::seconds(1), [] {
          return hw::hardware.block != hw::PlaybackBlock::Prepare;
        })) {
      return false;
    }
  }
  hw::hardware.prepared = !hw::hardware.fail_prepare;
  hw::hardware.playback_interrupted = false;
  return hw::hardware.prepared;
}
bool write(const std::int16_t* stereo, std::size_t frames) noexcept {
  std::unique_lock<std::mutex> lock(hw::hardware.mutex);
  if (frames == 0) {
    return !hw::hardware.playback_interrupted;
  }
  if (stereo == nullptr) {
    return false;
  }
  ++hw::hardware.writes;
  hw::hardware.operations.push_back("playback.write");
  hw::hardware.changed.notify_all();
  if (!hw::playback_wait(hw::PlaybackBlock::Write, lock)) {
    return false;
  }
  // 用实际48k样本时长模拟声卡消费；取消必须打断等待，不能靠测试超时放行。
  if (hw::hardware.changed.wait_for(lock, std::chrono::microseconds(frames * 1000000 / 48000),
                                    [] {
                                      return hw::hardware.playback_interrupted;
                                    })) {
    return false;
  }
  hw::hardware.written.insert(hw::hardware.written.end(), stereo, stereo + frames * 2);
  return true;
}
bool drain() noexcept {
  std::unique_lock<std::mutex> lock(hw::hardware.mutex);
  ++hw::hardware.drains;
  hw::hardware.operations.push_back("playback.drain");
  return hw::playback_wait(hw::PlaybackBlock::Drain, lock);
}
void drop() noexcept {
  std::lock_guard<std::mutex> lock(hw::hardware.mutex);
  hw::hardware.owner_valid &= std::this_thread::get_id() != hw::hardware.capture_thread;
  hw::hardware.prepared = false;
  hw::hardware.operations.push_back("playback.drop");
}
void interrupt_capture() noexcept {
  std::lock_guard<std::mutex> lock(hw::hardware.mutex);
  hw::hardware.capture_interrupted = true;
  ++hw::hardware.interrupts;
  hw::hardware.operations.push_back("capture.interrupt");
  hw::hardware.changed.notify_all();
}
void interrupt_playback() noexcept {
  std::lock_guard<std::mutex> lock(hw::hardware.mutex);
  hw::hardware.playback_interrupted = true;
  hw::hardware.operations.push_back("playback.interrupt");
  hw::hardware.changed.notify_all();
}
bool playback_interrupted() noexcept {
  std::lock_guard<std::mutex> lock(hw::hardware.mutex);
  return hw::hardware.playback_interrupted;
}
std::string capture_error() {
  std::lock_guard<std::mutex> lock(hw::hardware.mutex);
  return hw::hardware.fail_capture_open ? "ALSA capture open: injected failure" : "";
}
std::string playback_error() {
  std::lock_guard<std::mutex> lock(hw::hardware.mutex);
  return hw::hardware.fail_prepare ? "ALSA playback prepare: injected failure" : "";
}
void clear_playback_error() noexcept {}
void close_capture() noexcept {
  std::lock_guard<std::mutex> lock(hw::hardware.mutex);
  hw::hardware.capture_open = false;
  hw::hardware.operations.push_back("capture.close");
  hw::hardware.changed.notify_all();
}
void close_playback() noexcept {
  std::lock_guard<std::mutex> lock(hw::hardware.mutex);
  hw::hardware.playback_open = false;
  hw::hardware.operations.push_back("playback.close");
  hw::hardware.changed.notify_all();
}
}  // namespace boompi::alsa_audio
