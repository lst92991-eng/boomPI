#include "boompi/audio/voice_input.h"

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <thread>

#include "alsa_audio.h"
#include "audio_convert.h"
#include "audio_thread.h"
#include "board_voice_profile.h"
#include "boompi/platform/rv1106/rockchip_3a.h"
#include "frame_queue.h"

namespace boompi::voice_input {
namespace {
constexpr std::size_t kCaptureSlots = audio::VoiceFrameContract::FramesForMs(80U);
std::mutex mutex;
std::condition_variable condition;
std::thread thread;
std::atomic<bool> stop{false};
bool opened{false}, failed{false};
std::array<char, 192U> failure{};
audio::FrameQueue<audio::CaptureFrame, kCaptureSlots> frames;
// 大块工作内存随模块预分配，采集实时循环不分配或借用应用缓冲。
audio::RawCaptureFrame raw;
audio::CaptureChannels channels;

std::uint64_t sequence{0U};

bool Fail(const char* reason) {
  std::lock_guard<std::mutex> lock(mutex);
  std::snprintf(failure.data(), failure.size(), "%s", reason);
  failed = true;
  condition.notify_all();
  return false;
}

bool ResetFrontEnd() {
  if (!audio_convert::reset_capture()) {
    return Fail("capture resampler reset failed");
  }
  rockchip_3a::close();
  if (!rockchip_3a::open(audio::board::kAecDelaySamples)) {
    return Fail("Rockchip 3A initialization failed");
  }
  return true;
}

void CaptureTask() {
  audio::SetAudioThreadPriority("boompi-capture", 40);
  while (!stop.load()) {
    // 原始读取 → 联合降采样 → 3A → 交付；这里不接收网络或业务命令。
    raw.discontinuity = false;
    if (!alsa_audio::read(raw.pcm.data(), &raw.discontinuity)) {
      if (!stop.load()) {
        Fail(alsa_audio::capture_error().c_str());
      }
      break;
    }
    if (stop.load()) {
      break;
    }
    raw.timestamp_us =
        static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                       std::chrono::steady_clock::now().time_since_epoch())
                                       .count());
    audio::CaptureFrame frame{};
    if (raw.discontinuity) {
      if (!ResetFrontEnd()) {
        break;
      }
      frame.discontinuity = true;
    } else {
      if (!audio_convert::capture(raw, &channels)) {
        Fail("capture resampler lost frame alignment");
        break;
      }
      audio::CaptureMetadata metadata;
      if (!rockchip_3a::process(channels, &frame.pcm, &metadata)) {
        Fail("Rockchip 3A rejected a frame");
        break;
      }
      frame.timestamp_us = metadata.timestamp_us;
      frame.input_dbfs = metadata.input_dbfs;
      frame.reference_active = metadata.reference_active;
    }
    // 队列满显式发布断点，应用必须取消残缺输入，不能悄悄跳过PCM。
    frame.output = playback::observe();
    std::lock_guard<std::mutex> lock(mutex);
    frame.sequence = sequence++;
    if (frame.discontinuity || frames.Size() == kCaptureSlots) {
      frame.actor_overrun = !frame.discontinuity;
      frame.discontinuity = true;
      frames.Clear();
    }
    static_cast<void>(frames.Push(frame));
    condition.notify_all();
  }
}

}  // namespace

bool open() {
  if (opened) {
    return false;
  }
  failure.fill('\0');
  failed = false;
  if (!alsa_audio::open_capture(audio::kCapturePcm)) {
    return Fail(alsa_audio::capture_error().c_str());
  }
  if (!audio_convert::open_capture(audio::board::kLeftMicPolarity,
                                   audio::board::kRightMicPolarity)) {
    close();
    return Fail("capture resampler initialization failed");
  }
  if (!ResetFrontEnd()) {
    close();
    return false;
  }
  stop.store(false);
  frames.Clear();
  sequence = 0U;
  opened = true;
  return true;
}

bool start() {
  // 由应用在两个PCM均配置成功后启动；不假设Mode1允许输出open之前先read。
  if (!opened || thread.joinable() || failed) {
    return false;
  }
  try {
    thread = std::thread(CaptureTask);
  } catch (...) {
    close();
    return Fail("capture thread creation failed");
  }
  return true;
}

ReadResult read(audio::CaptureFrame* frame, std::chrono::milliseconds timeout) {
  if (frame == nullptr || timeout < std::chrono::milliseconds::zero()) {
    return ReadResult::Failed;
  }
  std::unique_lock<std::mutex> lock(mutex);
  if (!opened || !condition.wait_for(lock, timeout, [] {
        return stop.load() || failed || frames.Size() != 0U;
      })) {
    return opened ? ReadResult::Timeout : ReadResult::Failed;
  }
  if (stop.load() || failed) {
    return ReadResult::Failed;
  }
  static_cast<void>(frames.Pop(frame));
  return ReadResult::Frame;
}
std::string error() {
  std::lock_guard<std::mutex> lock(mutex);
  return failure[0] != '\0' ? std::string(failure.data()) : alsa_audio::capture_error();
}
void close() {
  stop.store(true);
  {
    std::lock_guard<std::mutex> lock(mutex);
    condition.notify_all();
  }
  alsa_audio::interrupt_capture();
  if (thread.joinable()) {
    thread.join();
  }
  alsa_audio::close_capture();
  audio_convert::close_capture();
  rockchip_3a::close();
  std::lock_guard<std::mutex> lock(mutex);
  opened = false;
  frames.Clear();
}
}  // namespace boompi::voice_input
