#include "boompi/audio/playback.h"

#include <alsa/asoundlib.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>

#include "audio_convert.h"
#include "audio_thread.h"
#include "board_voice_profile.h"
#include "boompi/debug.h"

namespace boompi::playback {
namespace {
constexpr std::size_t kCapacity = audio::kVoiceRateHz * 1500 / 1000;
std::array<std::int16_t, kCapacity> samples;
std::size_t head{0}, buffered{0};
std::mutex mutex;
std::condition_variable ready;
std::thread thread;
snd_pcm_t* device{nullptr};
std::atomic<bool> stopping{false}, canceled{false};
std::atomic<std::uint8_t> volume{60};
State state{State::Idle};
bool ending{false}, holding{false};
std::chrono::steady_clock::time_point hold_until{};
std::atomic<bool> hold_applied{false};
bool hold_active() {
  return holding && std::chrono::steady_clock::now() < hold_until;
}
char failure[192]{};
audio::StereoPlaybackFrame stereo;

// 启动时尚无工作线程；运行时所有错误写入均持mutex，保留最初失败原因。
bool fail(const char* stage, int code = 0) {
  if (!failure[0]) {
    std::snprintf(failure, sizeof(failure), code ? "%s (%d)" : "%s", stage, code);
  }
  return false;
}
int render(const std::int16_t* pcm, std::size_t samples, bool silence = false) {
  if (silence) {
    // 不把静音送进TTS重采样器；保留其历史和未消费的TTS，恢复时不吞字。
    stereo.pcm.fill(0);
    stereo.frames = audio::kDeviceFrameSamples;
  } else if (!audio_convert::playback(pcm, samples, stereo)) {
    return -EIO;
  }
  long peak = 0;
  for (std::size_t i = 0; i < stereo.frames * 2; ++i) {
    peak = std::max(peak, std::abs(static_cast<long>(stereo.pcm[i])));
  }
  // 百分比音量只在这里应用；整块峰值限制在95%满幅，-32768也不会溢出。
  float gain = volume.load() / 100.0F;
  if (peak) {
    gain = std::min(gain, 31128.0F / static_cast<float>(peak));
  }
  for (std::size_t i = 0; i < stereo.frames * 2; ++i) {
    stereo.pcm[i] = static_cast<std::int16_t>(stereo.pcm[i] * gain);
  }
  std::size_t offset = 0;
  while (offset < stereo.frames) {
    if (canceled.load() || stopping.load()) {
      return -ECANCELED;
    }
    const int count = static_cast<int>(
        snd_pcm_writei(device, stereo.pcm.data() + 2 * offset, stereo.frames - offset));
    if (count > 0) {
      offset += static_cast<std::size_t>(count);
    } else if (canceled.load() && (count == -EBADFD || count == -EINTR || count == -EPIPE)) {
      return -ECANCELED;
    } else if (count == -EPIPE || count == -ESTRPIPE) {
      const int result = snd_pcm_prepare(device);
      if (result < 0) {
        return result;
      }
      // 恢复只写尚未接受的后缀，不能重播前半帧。
      debug::log.playback_xrun(offset, count);
    } else if (count != -EINTR) {
      return count < 0 ? count : -EIO;
    }
  }
  return static_cast<int>(stereo.frames);
}
void drop() {
  const int result = snd_pcm_drop(device);
  if (result < 0 && result != -EBADFD) {
    fail("playback drop failed", result);
  }
}

void play() {
  audio::SetAudioThreadPriority("boompi-playback", 30);
  while (!stopping.load()) {
    std::unique_lock<std::mutex> lock(mutex);
    ready.wait(lock, [] {
      return stopping.load() || canceled ||
             (state == State::Playing && (ending || buffered != 0));
    });
    if (stopping.load()) {
      break;
    }
    int result = canceled ? -ECANCELED : snd_pcm_prepare(device);
    if (result >= 0 && !audio_convert::reset_playback()) {
      result = -EIO;
    }
    while (result >= 0 && !canceled && !stopping.load()) {
      ready.wait(lock, [] {
        const bool quiet = hold_active();
        if (!quiet) {
          hold_applied.store(false);
        }
        return stopping.load() || canceled || quiet || ending ||
               buffered >= audio::kVoiceFrameSamples;
      });
      if (!canceled && !stopping.load() && hold_active()) {
        lock.unlock();
        result = render(nullptr, 0, true);
        lock.lock();
        hold_applied.store(result >= 0 && hold_active() && !canceled);
        continue;
      }
      hold_applied.store(false);
      if (canceled || stopping.load() || buffered == 0) {
        break;
      }
      audio::VoiceFrame16k frame;
      const auto count = std::min(buffered, frame.size());
      for (std::size_t i = 0; i < count; ++i) {
        frame[i] = samples[(head + i) % kCapacity];
      }
      head = (head + count) % kCapacity;
      buffered -= count;
      lock.unlock();
      // 采样环 → 必要重采样/音量 → 声卡。起播缓冲交给ALSA，软件不再叠加180ms等待。
      result = render(frame.data(), count);
      lock.lock();
    }
    bool discard = canceled || stopping.load() || result == -ECANCELED;
    lock.unlock();
    if (result >= 0 && !discard) {
      // 正常结束写完滤波尾音再drain；取消时绝不把旧尾音补到下一次播放。
      do {
        result = render(nullptr, 0);
      } while (result > 0);
      if (result == 0) {
        result = snd_pcm_drain(device);
        if (canceled.load() && (result == -EBADFD || result == -EINTR)) {
          result = -ECANCELED;
        }
      }
    }
    lock.lock();
    discard = discard || canceled || stopping.load() || result == -ECANCELED;
    if (result < 0 && result != -ECANCELED) {
      fail("playback render/drain failed", result);
    }
    if (discard || failure[0]) {
      drop();
    }
    state = discard ? State::Idle : State::Drained;
    buffered = 0;
    ending = canceled = holding = false;
    hold_applied.store(false);
    ready.notify_all();
  }
}
}  // namespace

bool open(std::uint8_t level) {
  failure[0] = '\0';
  int result = snd_pcm_open(
      &device, audio::kPlaybackPcm, SND_PCM_STREAM_PLAYBACK,
      SND_PCM_NO_AUTO_RESAMPLE | SND_PCM_NO_AUTO_CHANNELS | SND_PCM_NO_AUTO_FORMAT);
  if (result >= 0) {
    result = snd_pcm_set_params(device, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED,
                                audio::kPlaybackChannels, audio::kDeviceRateHz, 0, 80000);
  }
  if (result < 0) {
    close();
    return fail("playback open/configuration failed", result);
  }
  if (!audio_convert::open_playback()) {
    close();
    return fail("playback resampler initialization failed");
  }
  set_volume(level);
  state = State::Idle;
  stopping.store(false);
  try {
    thread = std::thread(play);
    return true;
  } catch (...) {
    close();
    return fail("playback thread creation failed");
  }
}
WriteResult write(const void* data, std::size_t bytes) {
  const auto* pcm = static_cast<const std::uint8_t*>(data);
  if (!pcm || !bytes || bytes % 2) {
    return WriteResult::InvalidArgument;
  }
  std::unique_lock<std::mutex> lock(mutex);
  if (canceled && !ready.wait_for(lock, std::chrono::milliseconds(60), [] {
        return state != State::Playing || stopping.load();
      })) {
    fail("playback cancel timed out");
    return WriteResult::Rejected;
  }
  if (!thread.joinable() || stopping.load() || failure[0]) {
    return WriteResult::Rejected;
  }
  if (state == State::Playing && ending) {
    return WriteResult::Rejected;
  }
  const auto count = bytes / 2;
  if (count > kCapacity - buffered) {
    return WriteResult::Full;
  }
  if (state != State::Playing) {
    state = State::Playing;
    ending = false;
  }
  // 网络包边界在这里结束；播放器只保存连续采样，不再给每一包附加长度状态。
  for (std::size_t i = 0; i < count; ++i) {
    samples[(head + buffered + i) % kCapacity] =
        static_cast<std::int16_t>(pcm[2 * i] | (static_cast<unsigned>(pcm[2 * i + 1]) << 8));
  }
  buffered += count;
  ready.notify_all();
  return WriteResult::Queued;
}
void finish() {
  std::lock_guard<std::mutex> lock(mutex);
  ending = true;
  ready.notify_all();
}
void cancel() {
  std::lock_guard<std::mutex> lock(mutex);
  holding = false;
  hold_applied.store(false);
  if (state == State::Playing) {
    canceled = true;
    buffered = 0;
    drop();
    ready.notify_all();
  }
}
void hold(bool enabled) {
  std::lock_guard<std::mutex> lock(mutex);
  if (enabled && !holding) {
    // 为试探期间的新下行预留空间；积压时不再暂停消费，避免误试探挤满播放环。
    const bool room = buffered <= kCapacity - audio::kVoiceRateHz * 700 / 1000;
    hold_until = std::chrono::steady_clock::now() + std::chrono::milliseconds(room ? 500 : 0);
  }
  holding = enabled && state == State::Playing && !canceled;
  if (!holding) {
    hold_applied.store(false);
  }
  ready.notify_all();
}
bool held() noexcept {
  return hold_applied.load();
}
void set_volume(std::uint8_t level) {
  volume.store(std::min<std::uint8_t>(level, 100));
}
State status() {
  std::lock_guard<std::mutex> lock(mutex);
  return failure[0] ? State::Failed : state;
}
std::string error() {
  std::lock_guard<std::mutex> lock(mutex);
  return failure;
}
void close() {
  stopping.store(true);
  cancel();
  ready.notify_all();
  if (thread.joinable()) {
    thread.join();
  }
  if (device) {
    snd_pcm_close(device);
    device = nullptr;
  }
  audio_convert::close_playback();
  std::lock_guard<std::mutex> lock(mutex);
  state = State::Idle;
  buffered = 0;
  ending = canceled = holding = false;
  hold_applied.store(false);
}
}  // namespace boompi::playback
