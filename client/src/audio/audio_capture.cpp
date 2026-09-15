#include "boompi/audio/audio_capture.h"

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
#include "playback_state.h"
#include "vad.h"
#include "wake.h"

namespace boompi::audio_capture {
namespace {
constexpr std::size_t kCaptureSlots = audio::VoiceFrameContract::FramesForMs(80U);
constexpr auto kCommandTimeout = std::chrono::milliseconds(100);
enum class Command { None, ResetListener, ArmPlayback };
std::mutex mutex;
std::condition_variable condition;
std::thread thread;
std::atomic<bool> stop{false};
bool opened{false}, failed{false}, command_succeeded{false};
Command command{Command::None};
std::array<char, 192U> failure{};
audio::FrameQueue<audio::CaptureFrame, kCaptureSlots> frames;
// 大块工作内存随模块预分配，采集实时循环不分配或借用应用缓冲。
audio::RawCaptureFrame raw;
audio::CaptureChannels channels;
audio::CleanAudioFrame clean;
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
  if (!wake::reset() || !vad::reset()) {
    return Fail("capture detector reset failed");
  }
  return true;
}

bool ApplyCommand() {
  Command pending;
  {
    std::lock_guard<std::mutex> lock(mutex);
    if (failed) {
      return false;
    }
    pending = command;
  }
  if (pending == Command::None) {
    return true;
  }
  bool succeeded = true;
  if (pending == Command::ResetListener) {
    succeeded = wake::reset() && vad::reset();
  } else {
    vad::arm_playback();
  }
  {
    std::lock_guard<std::mutex> lock(mutex);
    if (pending == Command::ResetListener) {
      // 保留PCM时间线，只清命令前生成的唤醒/VAD结果。
      for (std::size_t i = 0U; i < frames.Size(); ++i) {
        auto& frame = frames[i];
        frame.wake = frame.vad_now = frame.vad_started = frame.vad_ended = false;
        frame.near_voice = false;
      }
    }
    command_succeeded = succeeded;
    command = Command::None;
    condition.notify_all();
  }
  return succeeded || Fail("capture detector command failed");
}

void CaptureTask() {
  audio::SetAudioThreadPriority("boompi-capture", 40);
  while (!stop.load() && ApplyCommand()) {
    // 读取四槽原始输入 → 联合降采样 → 3A → 唤醒 → VAD → 发布。
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
      vad::discontinuity();
      if (!ResetFrontEnd()) {
        break;
      }
      frame.discontinuity = true;
    } else {
      if (!audio_convert::capture(raw, &channels)) {
        Fail("capture resampler lost frame alignment");
        break;
      }
      if (!rockchip_3a::process(channels, &clean)) {
        Fail("Rockchip 3A rejected a frame");
        break;
      }
      frame.pcm = clean.pcm;
      frame.timestamp_us = clean.metadata.timestamp_us;
      frame.input_dbfs = clean.metadata.input_dbfs;
      frame.reference_active = clean.metadata.reference_active;
      if (!wake::detect(frame.pcm, &frame.wake)) {
        Fail(wake::error());
        break;
      }
      if (!vad::process(&frame, playback::observe())) {
        Fail(vad::error());
        break;
      }
    }
    // 队列满显式发布断点，应用必须取消残缺输入，不能悄悄跳过PCM。
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

bool RunCommand(Command request) {
  std::unique_lock<std::mutex> lock(mutex);
  if (!opened || stop.load() || failed || command != Command::None) {
    return false;
  }
  command = request;
  condition.notify_all();
  if (!condition.wait_for(lock, kCommandTimeout, [] {
        return stop.load() || failed || command == Command::None;
      })) {
    failed = true;
    std::snprintf(failure.data(), failure.size(), "capture control timed out");
    condition.notify_all();
    return false;
  }
  return !stop.load() && !failed && command_succeeded;
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
  if (!wake::open() || !vad::open()) {
    const std::string reason = wake::error()[0] != '\0' ? wake::error() : vad::error();
    close();
    return Fail(reason.c_str());
  }
  if (!ResetFrontEnd()) {
    close();
    return false;
  }
  stop.store(false);
  command = Command::None;
  command_succeeded = false;
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
bool reset_listener() {
  return RunCommand(Command::ResetListener);
}
bool arm_playback() {
  return RunCommand(Command::ArmPlayback);
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
  wake::close();
  vad::close();
  std::lock_guard<std::mutex> lock(mutex);
  opened = false;
  frames.Clear();
  command = Command::None;
}
}  // namespace boompi::audio_capture
