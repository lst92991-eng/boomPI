#include "audio_backend.h"
#include "boompi/audio/audio_engine.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <future>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;
using boompi::audio::AudioEngine;
using boompi::audio::AudioEngineConfig;
using boompi::audio::CaptureFrame;
using boompi::audio::CaptureResult;
using boompi::audio::QueueTtsResult;
namespace fake = boompi::test::audio_backend;

bool Check(const bool condition, const char* const message) {
  if (condition) return true;
  std::fprintf(stderr, "audio-engine harness: %s\n", message);
  return false;
}

bool WaitForPlaybackDone(const AudioEngine& engine,
                         const std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (!engine.playback_done() && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(1ms);
  return engine.playback_done();
}

bool OpenEngine(AudioEngine* const engine) {
  fake::Reset();
  AudioEngineConfig config;
  config.playback_gain = 1.0F;
  return Check(engine->Open(config), "Open failed") &&
         Check(fake::WaitForCaptureReads(1U, 500ms),
               "capture actor did not enter its blocking read");
}

bool BeginPlayback(AudioEngine* const engine) {
  std::promise<void> entered;
  auto entered_future = entered.get_future();
  auto begin = std::async(std::launch::async,
                          [engine, entered = std::move(entered)]() mutable {
                            entered.set_value();
                            return engine->BeginPlayback();
                          });
  entered_future.wait();
  // The real backend returns from ALSA every 20 ms. One fake capture period
  // gives the actor the same frame boundary at which it applies the command.
  for (std::size_t attempt = 0U; attempt < 10U &&
       begin.wait_for(0ms) != std::future_status::ready; ++attempt) {
    fake::PushCapture(CaptureFrame{});
    std::this_thread::sleep_for(5ms);
  }
  if (!Check(begin.wait_for(500ms) == std::future_status::ready,
             "BeginPlayback did not complete at a capture boundary"))
    return false;
  return Check(begin.get(), "BeginPlayback failed");
}

std::vector<std::uint8_t> Pcm24(const std::size_t samples,
                                const std::int16_t value) {
  std::vector<std::uint8_t> bytes(samples * 2U);
  const auto bits = static_cast<std::uint16_t>(value);
  for (std::size_t i = 0U; i < samples; ++i) {
    bytes[2U * i] = static_cast<std::uint8_t>(bits & 0xffU);
    bytes[2U * i + 1U] = static_cast<std::uint8_t>(bits >> 8U);
  }
  return bytes;
}

bool QueueFrame(AudioEngine* const engine, const std::uint64_t sequence,
                const std::size_t samples = 480U) {
  const auto bytes = Pcm24(samples, static_cast<std::int16_t>(sequence + 1U));
  return Check(engine->QueueTts24k(bytes.data(), bytes.size(), sequence) ==
                   QueueTtsResult::kQueued,
               "QueueTts24k failed");
}

bool PrimePlayback(AudioEngine* const engine) {
  if (!BeginPlayback(engine)) return false;
  for (std::uint64_t sequence = 0U; sequence < 9U; ++sequence) {
    if (!QueueFrame(engine, sequence)) return false;
  }
  return Check(fake::WaitForRenderCalls(9U, 500ms),
               "initial 180 ms buffer was not rendered");
}

bool TestSubGraceJitter() {
  AudioEngine engine;
  if (!OpenEngine(&engine) || !PrimePlayback(&engine)) return false;

  for (std::uint64_t sequence = 9U; sequence < 12U; ++sequence) {
    std::this_thread::sleep_for(5ms);
    if (!QueueFrame(&engine, sequence) ||
        !Check(fake::WaitForRenderCalls(static_cast<std::size_t>(sequence + 1U),
                                        100ms),
               "a sub-30 ms gap incorrectly required two packets"))
      return false;
  }

  if (!Check(engine.EndPlayback(), "EndPlayback failed") ||
      !Check(WaitForPlaybackDone(engine, 500ms), "playback did not finish"))
    return false;
  return Check(fake::RenderCallsSnapshot().size() == 12U,
               "continuous playback rendered an unexpected frame count");
}

bool TestRebufferAfterConfirmedGap() {
  AudioEngine engine;
  if (!OpenEngine(&engine) || !PrimePlayback(&engine)) return false;

  std::this_thread::sleep_for(60ms);
  if (!QueueFrame(&engine, 9U)) return false;
  std::this_thread::sleep_for(15ms);
  if (!Check(fake::RenderCallsSnapshot().size() == 9U,
             "confirmed underrun resumed from only one 20 ms packet"))
    return false;
  if (!QueueFrame(&engine, 10U) ||
      !Check(fake::WaitForRenderCalls(11U, 200ms),
             "two buffered packets did not resume playback"))
    return false;

  return Check(engine.EndPlayback(), "EndPlayback failed") &&
         Check(WaitForPlaybackDone(engine, 500ms), "playback did not finish");
}

bool TestEndPlaybackShortTail() {
  AudioEngine engine;
  if (!OpenEngine(&engine) || !PrimePlayback(&engine)) return false;

  std::this_thread::sleep_for(60ms);
  if (!QueueFrame(&engine, 9U, 240U) ||
      !Check(engine.EndPlayback(), "EndPlayback rejected a short tail") ||
      !Check(WaitForPlaybackDone(engine, 500ms),
             "short tail was stranded behind the rebuffer threshold"))
    return false;
  const auto calls = fake::RenderCallsSnapshot();
  return Check(calls.size() == 10U, "short tail was not rendered") &&
         Check(calls.back().pcm.size() == 240U,
               "short tail size changed before reaching the backend");
}

bool TestBoundedClose() {
  AudioEngine engine;
  if (!OpenEngine(&engine)) return false;

  const auto started = std::chrono::steady_clock::now();
  engine.Close();
  const auto elapsed = std::chrono::steady_clock::now() - started;
  return Check(elapsed < 200ms, "Close waited on a blocked capture read") &&
         Check(fake::WaitForCaptureInterrupts(1U, 20ms),
               "Close did not call InterruptCapture");
}

bool TestBoundedCaptureWait() {
  AudioEngine engine;
  if (!OpenEngine(&engine)) return false;

  CaptureFrame frame{};
  const auto started = std::chrono::steady_clock::now();
  const CaptureResult result = engine.Capture(&frame, 40ms);
  const auto elapsed = std::chrono::steady_clock::now() - started;
  return Check(result == CaptureResult::kTimeout,
               "blocked capture was not reported as a timeout") &&
         Check(elapsed >= 30ms && elapsed < 150ms,
               "capture timeout was outside its bounded window");
}

bool TestBoundedCaptureCommand() {
  AudioEngine engine;
  if (!OpenEngine(&engine)) return false;

  const auto started = std::chrono::steady_clock::now();
  const bool began = engine.BeginPlayback();
  const auto elapsed = std::chrono::steady_clock::now() - started;
  return Check(!began, "capture-bound command unexpectedly succeeded") &&
         Check(elapsed >= 80ms && elapsed < 250ms,
               "capture-bound command did not fail in its bounded window");
}

bool TestResetPreservesQueuedPcm() {
  AudioEngine engine;
  if (!OpenEngine(&engine)) return false;

  for (std::size_t index = 0U; index < 3U; ++index) {
    CaptureFrame frame{};
    frame.pcm[0] = static_cast<std::int16_t>(101U + index);
    frame.timestamp_us = 1000000U + index * 20000U;
    frame.input_dbfs = -20.0F - static_cast<float>(index);
    frame.wake = frame.vad_now = frame.vad_started = frame.vad_ended = true;
    frame.near_voice = frame.reference_active = true;
    frame.discontinuity = index == 0U;
    fake::PushCapture(frame);
  }
  if (!Check(fake::WaitForProcessedFrames(3U, 500ms),
             "three listener frames were not processed") ||
      !Check(fake::WaitForCaptureReads(4U, 500ms),
             "capture actor did not block after the queued frames"))
    return false;

  auto reset = std::async(std::launch::async,
                          [&engine] { return engine.ResetListener(); });
  std::this_thread::sleep_for(10ms);
  CaptureFrame fourth{};
  fourth.pcm[0] = 104;
  fourth.timestamp_us = 1060000U;
  fourth.input_dbfs = -23.0F;
  fourth.wake = fourth.vad_now = fourth.vad_started = fourth.vad_ended = true;
  fourth.near_voice = fourth.reference_active = true;
  fake::PushCapture(fourth);
  if (!Check(reset.wait_for(500ms) == std::future_status::ready,
             "ResetListener did not complete at a capture boundary") ||
      !Check(reset.get(), "ResetListener failed"))
    return false;

  for (std::size_t index = 0U; index < 4U; ++index) {
    CaptureFrame actual{};
    if (!Check(engine.Capture(&actual, 100ms) == CaptureResult::kFrame,
               "queued PCM disappeared at reset") ||
        !Check(actual.sequence == index, "capture sequence changed at reset") ||
        !Check(actual.pcm[0] == static_cast<std::int16_t>(101U + index),
               "queued PCM changed at reset") ||
        !Check(actual.timestamp_us == 1000000U + index * 20000U,
               "capture timestamp changed at reset") ||
        !Check(actual.input_dbfs == -20.0F - static_cast<float>(index),
               "dBFS evidence changed at reset") ||
        !Check(actual.reference_active, "reference evidence changed at reset") ||
        !Check(!actual.wake && !actual.vad_now && !actual.vad_started &&
                   !actual.vad_ended && !actual.near_voice,
               "an old listener decision crossed the reset boundary"))
      return false;
    if (index == 0U &&
        !Check(actual.discontinuity,
               "capture discontinuity evidence changed at reset"))
      return false;
  }
  return true;
}

bool TestQueueResultsDoNotPolluteLastError() {
  AudioEngine engine;
  if (!OpenEngine(&engine)) return false;
  const auto frame = Pcm24(480U, 1);
  if (!Check(engine.QueueTts24k(nullptr, 0U, 0U) ==
                 QueueTtsResult::kInvalidArgument,
             "invalid PCM did not have a distinct queue result") ||
      !Check(engine.QueueTts24k(frame.data(), frame.size(), 0U) ==
                 QueueTtsResult::kNotActive,
             "inactive playback did not have a distinct queue result") ||
      !Check(engine.last_error().empty(),
             "transient queue rejection polluted last_error") ||
      !BeginPlayback(&engine))
    return false;

  const auto oversized = Pcm24(75U * 480U + 1U, 1);
  if (!Check(engine.QueueTts24k(oversized.data(), oversized.size(), 0U) ==
                 QueueTtsResult::kFull,
             "oversized PCM did not report a full queue") ||
      !Check(engine.QueueTts24k(frame.data(), frame.size(), 0U) ==
                 QueueTtsResult::kQueued,
             "valid PCM was not queued") ||
      !Check(engine.QueueTts24k(frame.data(), frame.size(), 2U) ==
                 QueueTtsResult::kDiscontinuous,
             "sequence gap did not have a distinct queue result") ||
      !Check(engine.last_error().empty(),
             "queue flow control polluted last_error"))
    return false;
  engine.DropPlayback();
  return Check(WaitForPlaybackDone(engine, 500ms),
               "dropped playback did not finish");
}

bool TestSuccessfulOpenClearsOldError() {
  AudioEngine engine;
  if (!OpenEngine(&engine) ||
      !Check(!engine.Open(AudioEngineConfig{}),
             "duplicate Open unexpectedly succeeded") ||
      !Check(!engine.last_error().empty(),
             "duplicate Open did not publish its error"))
    return false;
  engine.Close();
  return OpenEngine(&engine) &&
         Check(engine.last_error().empty(),
               "successful reopen retained an old error");
}

bool TestSuccessfulPlaybackClearsOldError() {
  AudioEngine engine;
  if (!OpenEngine(&engine) ||
      !Check(!engine.Open(AudioEngineConfig{}),
             "duplicate Open unexpectedly succeeded") ||
      !Check(!engine.last_error().empty(),
             "duplicate Open did not publish its error") ||
      !BeginPlayback(&engine))
    return false;
  const bool cleared = Check(engine.last_error().empty(),
                             "successful playback retained an old error");
  engine.DropPlayback();
  return cleared && Check(WaitForPlaybackDone(engine, 500ms),
                          "dropped playback did not finish");
}

}  // namespace

int main(const int argc, char** const argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: %s <scenario>\n", argv[0]);
    return 2;
  }
  const std::string scenario = argv[1];
  bool passed = false;
  if (scenario == "sub-grace-jitter") passed = TestSubGraceJitter();
  else if (scenario == "confirmed-gap") passed = TestRebufferAfterConfirmedGap();
  else if (scenario == "short-tail") passed = TestEndPlaybackShortTail();
  else if (scenario == "bounded-close") passed = TestBoundedClose();
  else if (scenario == "bounded-capture") passed = TestBoundedCaptureWait();
  else if (scenario == "bounded-command") passed = TestBoundedCaptureCommand();
  else if (scenario == "reset-preserves-pcm") passed = TestResetPreservesQueuedPcm();
  else if (scenario == "queue-results")
    passed = TestQueueResultsDoNotPolluteLastError();
  else if (scenario == "open-clears-error")
    passed = TestSuccessfulOpenClearsOldError();
  else if (scenario == "playback-clears-error")
    passed = TestSuccessfulPlaybackClearsOldError();
  else {
    std::fprintf(stderr, "unknown scenario: %s\n", scenario.c_str());
    return 2;
  }
  if (passed) std::printf("audio-engine harness: %s passed\n", scenario.c_str());
  return passed ? 0 : 1;
}
