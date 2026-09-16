/** @file voice_input.cpp
 * @brief 实时输入任务：ALSA → 格式适配 → Rockchip 3A → Snowboy → WebRTC VAD → 主线程。
 *
 * 算法在同一任务里顺序调用，只有最后交付处使用 4 帧（80ms）有界队列。
 * raw/channels 由采集任务独占；frames、读位置、错误原因在 mutex 下交接。
 */
#include "boompi/audio/voice_input.h"

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <exception>
#include <mutex>
#include <thread>

#include "audio_capture.h"
#include "audio_convert.h"
#include "audio_thread.h"
#include "board_voice_profile.h"
#include "boompi/audio/playback.h"
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

/** @brief 保存输入故障并唤醒主线程；应用读取 Failed 后结束运行。 */
bool fail(const char* reason, int code = 0) {
  std::lock_guard<std::mutex> lock(mutex);
  std::snprintf(failure.data(), failure.size(), code ? "%s (ALSA %d)" : "%s", reason, code);
  condition.notify_all();
  return false;
}

/** @brief 采集线程入口；每轮只读一块、处理一块、交付一块，不等待网络。 */
void capture_task() {
  audio::SetAudioThreadPriority("boompi-capture", 40);
  bool previous_reference = false, previous_held = false;
  for (;;) {
    // 1. 读取原始四槽 PCM；0 是断流，负值区分主动停止和设备故障。
    const bool held_before_read = playback::held();
    const int captured = audio_capture::read(raw.data());
    if (captured < 0) {
      if (captured != -ECANCELED) {
        fail("ALSA capture read failed", captured);
      }
      break;
    }
    audio::CaptureFrame frame{};
    if (captured == 0) {
      previous_reference = previous_held = false;
      rockchip_3a::close();
      if (!audio_convert::reset_capture() || !rockchip_3a::open() || !wake::reset() ||
          !vad::reset()) {
        fail("audio processing reset after discontinuity failed");
        break;
      }
      frame.discontinuity = true;
    } else {
      // Snowboy要求外部VAD句尾后Reset；只由本线程调用，不等待业务线程握手。
      if (wake_reset.exchange(false) && !wake::reset()) {
        fail("Snowboy reset failed");
        break;
      }
      // 2. 顺序处理这一帧：格式转换 → 3A → 唤醒 → VAD，算法之间不再放队列。
      if (!audio_convert::capture(raw, channels)) {
        fail("capture resampler lost frame alignment");
        break;
      }
      if (!rockchip_3a::process(channels, frame.pcm)) {
        fail("Rockchip 3A rejected a frame");
        break;
      }
      const int detected = wake::detect(frame.pcm);
      if (detected < 0) {
        fail("Snowboy processing failed");
        break;
      }
      frame.wake = detected > 0;
      const int voice = vad::process(frame.pcm);
      if (voice < 0) {
        fail("WebRTC VAD processing failed");
        break;
      }
      frame.vad_now = voice == 1;

      // 3. 给处理结果附带插话所需的观测；参考和播放观测随 3A 预填延后一帧。
      frame.reference_active = previous_reference;
      frame.playback_held = previous_held;
      previous_held = held_before_read;
      previous_reference = false;
      for (std::size_t i = 2; i < channels.size(); i += 3) {
        previous_reference = previous_reference || channels[i] > audio::board::kReferencePeak ||
                             channels[i] < -audio::board::kReferencePeak;
      }
    }
    // 4. 一次交付 PCM 和检测结果。队列满则发布断点，让应用取消残缺语句。
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
    return fail("ALSA capture initialization failed", result);
  }
  if (!audio_convert::open_capture()) {
    close();
    return fail("capture resampler initialization failed");
  }
  if (!rockchip_3a::open()) {
    close();
    return fail("Rockchip 3A initialization failed");
  }
  if (!wake::open()) {
    close();
    return fail("Snowboy initialization failed");
  }
  if (!vad::open()) {
    close();
    return fail("WebRTC VAD initialization failed");
  }
  wake_reset.store(false);
  read_at = pending = 0;
  return true;
}

bool start() {
  // 由应用在两个PCM均配置成功后启动；不假设Mode1允许输出open之前先read。
  // std::thread 创建会抛标准异常；失败在这里转为 false，并回收已经打开的输入资源。
  try {
    thread = std::thread(capture_task);
  } catch (const std::exception&) {
    close();
    return fail("capture thread creation failed");
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
    fail("ALSA capture stop failed", interrupted);
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
