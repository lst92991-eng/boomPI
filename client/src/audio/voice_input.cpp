#include "boompi/audio/voice_input.h"

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <thread>

#include "audio_capture.h"
#include "audio_convert.h"
#include "audio_thread.h"
#include "boompi/platform/rv1106/rockchip_3a.h"
#include "vad.h"
#include "wake.h"

namespace boompi::voice_input {
namespace {
constexpr std::size_t kCaptureSlots = 80 / audio::kFrameMs;
std::mutex mutex;
std::condition_variable condition;
std::thread thread;
std::atomic<bool> wake_reset{false};
std::array<char, 192U> failure{};
std::array<audio::CaptureFrame, kCaptureSlots> frames;
std::size_t read_at{0}, pending{0};
// 大块工作内存随模块预分配，采集实时循环不分配或借用应用缓冲。
audio::RawCaptureFrame raw;
audio::CaptureChannels channels;

bool Fail(const char* reason, int code = 0) {
  std::lock_guard<std::mutex> lock(mutex);
  std::snprintf(failure.data(), failure.size(), code ? "%s (ALSA %d)" : "%s", reason, code);
  condition.notify_all();
  return false;
}

void CaptureTask() {
  audio::SetAudioThreadPriority("boompi-capture", 40);
  for (;;) {
    // 读取 → 必要格式适配 → 3A → 唤醒 → VAD → 交付。
    const int captured = audio_capture::read(raw.data());
    if (captured < 0) {
      if (captured != -ECANCELED) {
        Fail("ALSA capture read failed", captured);
      }
      break;
    }
    audio::CaptureFrame frame{};
    if (captured == 0) {
      rockchip_3a::close();
      if (!audio_convert::reset_capture() || !rockchip_3a::open() || !wake::reset() ||
          !vad::reset()) {
        Fail("audio processing reset after discontinuity failed");
        break;
      }
      frame.discontinuity = true;
    } else {
      // Snowboy要求外部VAD句尾后Reset；只由本线程调用，不等待业务线程握手。
      if (wake_reset.exchange(false) && !wake::reset()) {
        Fail("Snowboy reset failed");
        break;
      }
      if (!audio_convert::capture(raw, channels)) {
        Fail("capture resampler lost frame alignment");
        break;
      }
      if (!rockchip_3a::process(channels, frame.pcm)) {
        Fail("Rockchip 3A rejected a frame");
        break;
      }
      const int detected = wake::detect(frame.pcm);
      if (detected < 0) {
        Fail("Snowboy processing failed");
        break;
      }
      frame.wake = detected > 0;
      const int voice = vad::process(frame.pcm);
      if (voice < 0) {
        Fail("WebRTC VAD processing failed");
        break;
      }
      frame.vad_now = voice == 1;
    }
    // 队列满显式发布断点，应用必须取消残缺输入，不能悄悄跳过PCM。
    std::lock_guard<std::mutex> lock(mutex);
    if (frame.discontinuity || pending == kCaptureSlots) {
      frame.discontinuity = true;
      read_at = pending = 0;
    }
    frames[(read_at + pending) % kCaptureSlots] = frame;
    ++pending;
    condition.notify_all();
  }
}

}  // namespace

bool open() {
  failure.fill('\0');
  const int result = audio_capture::open();
  if (result < 0) {
    return Fail("ALSA capture initialization failed", result);
  }
  if (!audio_convert::open_capture()) {
    close();
    return Fail("capture resampler initialization failed");
  }
  if (!rockchip_3a::open()) {
    close();
    return Fail("Rockchip 3A initialization failed");
  }
  if (!wake::open()) {
    close();
    return Fail("Snowboy initialization failed");
  }
  if (!vad::open()) {
    close();
    return Fail("WebRTC VAD initialization failed");
  }
  wake_reset.store(false);
  read_at = pending = 0;
  return true;
}

bool start() {
  // 由应用在两个PCM均配置成功后启动；不假设Mode1允许输出open之前先read。
  try {
    thread = std::thread(CaptureTask);
  } catch (...) {
    close();
    return Fail("capture thread creation failed");
  }
  return true;
}

ReadResult read(audio::CaptureFrame& frame) {
  std::unique_lock<std::mutex> lock(mutex);
  if (!condition.wait_for(lock, std::chrono::milliseconds(20), [] {
        return failure[0] || pending != 0;
      })) {
    return ReadResult::Timeout;
  }
  if (failure[0]) {
    return ReadResult::Failed;
  }
  frame = frames[read_at];
  read_at = (read_at + 1) % kCaptureSlots;
  --pending;
  return ReadResult::Frame;
}
void end_utterance() noexcept {
  wake_reset.store(true);
}
std::string error() {
  std::lock_guard<std::mutex> lock(mutex);
  return failure.data();
}
void close() {
  const int interrupted = audio_capture::interrupt();
  if (interrupted < 0) {
    Fail("ALSA capture stop failed", interrupted);
  }
  if (thread.joinable()) {
    thread.join();
  }
  audio_capture::close();
  audio_convert::close_capture();
  rockchip_3a::close();
  wake::close();
  vad::close();
  std::lock_guard<std::mutex> lock(mutex);
  read_at = pending = 0;
}
}  // namespace boompi::voice_input
