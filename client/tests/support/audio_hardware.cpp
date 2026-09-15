#include "audio_hardware.h"

#include <alsa/asoundlib.h>

#include <algorithm>
#include <cerrno>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

#include "audio_capture.h"

namespace boompi::test::audio_hardware {
namespace {
struct Hardware {
  std::mutex mutex;
  std::condition_variable changed;
  std::deque<std::pair<audio::RawCaptureFrame, bool>> captures;
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
void push_capture(const audio::RawCaptureFrame& frame, bool gap) {
  std::lock_guard<std::mutex> lock(hardware.mutex);
  hardware.captures.emplace_back(frame, gap);
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

namespace boompi::audio_capture {
namespace hw = test::audio_hardware;
int open() noexcept {
  std::lock_guard<std::mutex> lock(hw::hardware.mutex);
  hw::hardware.capture_open = !hw::hardware.fail_capture_open;
  hw::hardware.capture_interrupted = false;
  hw::hardware.operations.push_back("capture.open");
  return hw::hardware.capture_open ? 0 : -EIO;
}
int read(std::int16_t* output) noexcept {
  std::unique_lock<std::mutex> lock(hw::hardware.mutex);
  hw::hardware.capture_thread = std::this_thread::get_id();
  ++hw::hardware.reads;
  hw::hardware.changed.notify_all();
  hw::hardware.changed.wait(lock, [] {
    return !hw::hardware.capture_open || hw::hardware.capture_interrupted ||
           !hw::hardware.captures.empty();
  });
  if (!hw::hardware.capture_open || hw::hardware.capture_interrupted || output == nullptr) {
    return hw::hardware.capture_interrupted ? -ECANCELED : -EIO;
  }
  const auto frame = hw::hardware.captures.front();
  hw::hardware.captures.pop_front();
  std::copy(frame.first.begin(), frame.first.end(), output);
  return frame.second ? 0 : static_cast<int>(audio::kDeviceFrameSamples);
}
int interrupt() noexcept {
  std::lock_guard<std::mutex> lock(hw::hardware.mutex);
  hw::hardware.capture_interrupted = true;
  ++hw::hardware.interrupts;
  hw::hardware.operations.push_back("capture.interrupt");
  hw::hardware.changed.notify_all();
  return 0;
}
void close() noexcept {
  std::lock_guard<std::mutex> lock(hw::hardware.mutex);
  hw::hardware.capture_open = false;
  hw::hardware.operations.push_back("capture.close");
  hw::hardware.changed.notify_all();
}
}  // namespace boompi::audio_capture
namespace hw = boompi::test::audio_hardware;
extern "C" {
int snd_pcm_open(snd_pcm_t** output, const char*, snd_pcm_stream_t, int) {
  std::lock_guard<std::mutex> lock(hw::hardware.mutex);
  hw::hardware.playback_open = true;
  hw::hardware.playback_interrupted = false;
  hw::hardware.operations.push_back("playback.open");
  *output = reinterpret_cast<snd_pcm_t*>(&hw::hardware);
  return 0;
}
int snd_pcm_set_params(snd_pcm_t*, snd_pcm_format_t format, snd_pcm_access_t access,
                       unsigned channels, unsigned rate, int resample, unsigned) {
  return format == SND_PCM_FORMAT_S16_LE && access == SND_PCM_ACCESS_RW_INTERLEAVED &&
                 channels == 2 && rate == 48000 && resample == 0
             ? 0
             : -EINVAL;
}
int snd_pcm_prepare(snd_pcm_t*) {
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
      return hw::hardware.playback_interrupted ? -ECANCELED : -EIO;
    }
  }
  hw::hardware.prepared = !hw::hardware.fail_prepare;
  hw::hardware.playback_interrupted = false;
  return hw::hardware.prepared ? 0 : -EIO;
}
snd_pcm_sframes_t snd_pcm_writei(snd_pcm_t*, const void* data, snd_pcm_uframes_t frames) {
  const auto* stereo = static_cast<const std::int16_t*>(data);
  std::unique_lock<std::mutex> lock(hw::hardware.mutex);
  if (frames == 0) {
    return hw::hardware.playback_interrupted ? -ECANCELED : 0;
  }
  if (stereo == nullptr) {
    return hw::hardware.playback_interrupted ? -ECANCELED : -EIO;
  }
  ++hw::hardware.writes;
  hw::hardware.operations.push_back("playback.write");
  hw::hardware.changed.notify_all();
  if (!hw::playback_wait(hw::PlaybackBlock::Write, lock)) {
    return hw::hardware.playback_interrupted ? -ECANCELED : -EIO;
  }
  // 用实际48k样本时长模拟声卡消费；取消必须打断等待，不能靠测试超时放行。
  if (hw::hardware.changed.wait_for(lock, std::chrono::microseconds(frames * 1000000 / 48000),
                                    [] {
                                      return hw::hardware.playback_interrupted;
                                    })) {
    return hw::hardware.playback_interrupted ? -ECANCELED : -EIO;
  }
  hw::hardware.written.insert(hw::hardware.written.end(), stereo, stereo + frames * 2);
  return static_cast<snd_pcm_sframes_t>(frames);
}
int snd_pcm_drain(snd_pcm_t*) {
  std::unique_lock<std::mutex> lock(hw::hardware.mutex);
  ++hw::hardware.drains;
  hw::hardware.operations.push_back("playback.drain");
  return hw::playback_wait(hw::PlaybackBlock::Drain, lock) ? 0 : -EBADFD;
}
int snd_pcm_drop(snd_pcm_t*) {
  std::lock_guard<std::mutex> lock(hw::hardware.mutex);
  hw::hardware.playback_interrupted = true;
  hw::hardware.prepared = false;
  hw::hardware.operations.push_back("playback.drop");
  hw::hardware.changed.notify_all();
  return 0;
}
int snd_pcm_close(snd_pcm_t*) {
  std::lock_guard<std::mutex> lock(hw::hardware.mutex);
  hw::hardware.playback_open = false;
  hw::hardware.operations.push_back("playback.close");
  hw::hardware.changed.notify_all();
  return 0;
}
}
