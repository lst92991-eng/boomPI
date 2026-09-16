/** @file playback.cpp
 * @brief 播放任务：连续采样环 → 重采样/音量 → ALSA → 正常尾播完成通知。
 *
 * 主线程投递 PCM 和控制，播放线程消费；mutex 保护采样环与控制状态。
 * write/prepare/drain 由播放线程执行，cancel 可从主线程 drop 以打断阻塞输出。
 * 这里只管理音频消费，不决定新问题、追问窗口或网络轮次。
 */
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
#include <exception>
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
// ending 表示不会再有新采样；holding 是插话试探请求，两者都不等于声卡已静音。
bool ending{false}, holding{false};
std::chrono::steady_clock::time_point hold_until{};
std::atomic<bool> hold_applied{false};
char failure[192]{};
audio::StereoPlaybackFrame stereo;

/** @brief 持锁检查试探暂停是否尚未过期；重复 true 请求不会延长截止时间。 */
bool hold_active() {
  return holding && std::chrono::steady_clock::now() < hold_until;
}

// 启动时尚无工作线程；运行时所有错误写入均持mutex，保留最初失败原因。
bool fail(const char* stage, int code = 0) {
  if (!failure[0]) {
    std::snprintf(failure, sizeof(failure), code ? "%s (%d)" : "%s", stage, code);
  }
  return false;
}

/** @brief 对已准备的 stereo 应用音量并完整写声卡，不决定本块来自正文、静音或尾音。
 * 只由播放线程在不持队列锁时调用；部分写从后缀继续，取消返回 -ECANCELED。
 */
int write_device() {
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

/** @brief 正文已经交付完毕后，取出滤波尾音并等待声卡播完。
 * 只由播放线程无锁调用；取消会打断 write/drain，不把旧尾音留给下一轮。
 */
int drain_output() {
  for (;;) {
    if (!audio_convert::playback(nullptr, 0, stereo)) {
      return -EIO;
    }
    if (stereo.frames == 0) {
      break;
    }
    const int result = write_device();
    if (result < 0) {
      return result;
    }
  }
  const int result = snd_pcm_drain(device);
  if (canceled.load() && (result == -EBADFD || result == -EINTR)) {
    return -ECANCELED;
  }
  return result;
}

/** @brief 丢弃声卡尚未播放的数据；调用方持 mutex，保留除已停止状态以外的错误。 */
void drop() {
  const int result = snd_pcm_drop(device);
  if (result < 0 && result != -EBADFD) {
    fail("playback drop failed", result);
  }
}

/** @brief 等待输入 → 每块取出后解锁写声卡 → 尾播或取消 → 通知主线程。
 * 声卡写入可能阻塞，因此不能持队列锁；取消标志在每次写之前重新检查。
 */
void play() {
  audio::SetAudioThreadPriority("boompi-playback", 30);
  std::unique_lock<std::mutex> lock(mutex);
  while (!stopping.load()) {
    ready.wait(lock, [] {
      return stopping.load() || canceled.load() ||
             (state == State::Playing && (ending || buffered != 0));
    });
    if (stopping.load()) {
      break;
    }
    // 1. 新回答重新准备声卡及滤波历史；旧回答取消收尾前不会接纳新数据。
    int result = canceled.load() ? -ECANCELED : snd_pcm_prepare(device);
    if (result >= 0 && !audio_convert::reset_playback()) {
      result = -EIO;
    }
    while (result >= 0) {
      if (canceled.load() || stopping.load()) {
        result = -ECANCELED;
        break;
      }
      // 2. 插话试探期间直接写静音，采样环和转换器历史都保持原位。
      if (hold_active()) {
        stereo.pcm.fill(0);
        stereo.frames = audio::kDeviceFrameSamples;
        lock.unlock();
        result = write_device();
        lock.lock();
        hold_applied.store(result >= 0 && hold_active() && !canceled.load());
        continue;
      }
      hold_applied.store(false);
      // 3. 通常等完整 320 点；finish 后立即消费剩余短帧，队列为空才进入尾播。
      if (buffered < audio::kVoiceFrameSamples && !ending) {
        ready.wait(lock);
        continue;  // 唤醒后重新检查取消、暂停和数据量，不在等待条件里修改状态。
      }
      if (buffered == 0) {
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
      if (!audio_convert::playback(frame.data(), count, stereo)) {
        result = -EIO;
      } else {
        result = write_device();
      }
      lock.lock();
    }
    // 4. 只有正常结束才排尾音；解锁期间仍可取消，所以重新持锁后再决定最终状态。
    if (result >= 0 && !canceled.load() && !stopping.load()) {
      lock.unlock();
      result = drain_output();
      lock.lock();
    }
    const bool discard = canceled.load() || stopping.load() || result == -ECANCELED;
    if (result < 0 && result != -ECANCELED) {
      fail("playback render/drain failed", result);
    }
    if (discard || failure[0]) {
      drop();
    }
    state = discard ? State::Idle : State::Drained;
    buffered = 0;
    ending = holding = false;
    canceled.store(false);
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
  // std::thread 的失败通过标准异常返回；声卡与转换器仍按各自返回值判断。
  try {
    thread = std::thread(play);
    return true;
  } catch (const std::exception&) {
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
  if (canceled.load() && !ready.wait_for(lock, std::chrono::milliseconds(60), [] {
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
  // 整包接纳或整包失败；应用收到 Full 会取消本轮，不能保留一句缺字的回复。
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
    canceled.store(true);
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
  holding = enabled && state == State::Playing && !canceled.load();
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
  ending = holding = false;
  canceled.store(false);
  hold_applied.store(false);
}
}  // namespace boompi::playback
