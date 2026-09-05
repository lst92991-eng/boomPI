#include "audio_backend.h"
#include "boompi/audio/audio_engine.h"
#include "boompi/audio/voice_audio.h"

#include <array>
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
using boompi::audio::AudioEvent;
using boompi::audio::AudioEventKind;
using boompi::audio::VoiceAudio;
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

bool OpenVoice(VoiceAudio* const voice) {
  fake::Reset();
  return Check(voice->Open(60U), "VoiceAudio Open failed") &&
         Check(fake::WaitForCaptureReads(1U, 500ms),
               "VoiceAudio capture actor did not start");
}

bool BeginVoiceListen(VoiceAudio* const voice, const bool follow_up) {
  auto listen = std::async(std::launch::async,
                           [voice, follow_up] {
                             return voice->Listen(follow_up);
                           });
  for (std::size_t attempt = 0U; attempt < 10U &&
       listen.wait_for(0ms) != std::future_status::ready; ++attempt) {
    fake::PushCapture(CaptureFrame{});
    std::this_thread::sleep_for(5ms);
  }
  return Check(listen.wait_for(500ms) == std::future_status::ready,
               "VoiceAudio Listen did not cross a capture boundary") &&
         Check(listen.get(), "VoiceAudio Listen failed");
}

bool BeginVoicePlayback(VoiceAudio* const voice,
                        const std::uint32_t generation,
                        const std::vector<std::uint8_t>& pcm) {
  auto play = std::async(std::launch::async,
                         [voice, generation, &pcm] {
                           return voice->Play(generation, pcm.data(), pcm.size(),
                                              0U, true, false);
                         });
  for (std::size_t attempt = 0U; attempt < 20U &&
       play.wait_for(0ms) != std::future_status::ready; ++attempt) {
    fake::PushCapture(CaptureFrame{});
    std::this_thread::sleep_for(5ms);
  }
  return Check(play.wait_for(500ms) == std::future_status::ready,
               "VoiceAudio Play(start) did not complete") &&
         Check(play.get(), "VoiceAudio Play(start) failed");
}

bool PollInjected(VoiceAudio* const voice, CaptureFrame frame,
                  AudioEvent* const event) {
  fake::PushCapture(frame);
  return voice->Poll(event, 100ms);
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

bool TestPlaybackXrunCounter() {
  AudioEngine engine;
  if (!OpenEngine(&engine) || !PrimePlayback(&engine)) return false;
  fake::InjectPlaybackXruns(2U);
  if (!Check(engine.EndPlayback(), "EndPlayback failed after playback XRUN") ||
      !Check(WaitForPlaybackDone(engine, 500ms),
             "playback did not finish after recovered XRUN"))
    return false;
  return Check(fake::PlaybackXrunsSnapshot() == 2U,
               "playback XRUN counter did not preserve recovered events");
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
         Check(elapsed >= 30ms && elapsed < 500ms,
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

bool TestPlaybackOwnerOrder() {
  AudioEngine engine;
  if (!OpenEngine(&engine) || !PrimePlayback(&engine) || !engine.EndPlayback() ||
      !Check(WaitForPlaybackDone(engine, 500ms), "playback did not drain"))
    return false;
  // 再开一轮，证明播放准备只发生一次，且每轮都在本轮 capture 武装之后。
  return PrimePlayback(&engine) && engine.EndPlayback() &&
      Check(WaitForPlaybackDone(engine, 500ms), "second playback did not drain") &&
      Check(fake::PlaybackOwnerOrderIsValid(),
            "capture arm or playback prepare/render/drain crossed its owner");
}

bool TestInterruptBlockedPlayback(fake::PlaybackBlock stage, bool close) {
  AudioEngine engine;
  if (!OpenEngine(&engine) || !BeginPlayback(&engine)) return false;
  fake::BlockPlayback(stage);
  // EOS 放行一个短回复，同时覆盖 render 与随后 drain 的阻塞位置。
  if (!QueueFrame(&engine, 0U) || !engine.EndPlayback() ||
      !Check(fake::WaitForPlaybackBlocked(500ms), "playback did not reach blocking I/O"))
    return false;
  const auto started = std::chrono::steady_clock::now();
  if (close) engine.Close();
  else engine.DropPlayback();
  return Check(WaitForPlaybackDone(engine, 200ms), "interrupt did not release playback") &&
      Check(std::chrono::steady_clock::now() - started < 250ms,
            "drop/close waited for blocked playback") &&
      Check(!engine.playback_failed(), "user interruption became playback failure") &&
      Check(fake::PlaybackOwnerOrderIsValid(), "interruption changed playback owner");
}

bool TestPlaybackPrepareFailure() {
  AudioEngine engine;
  if (!OpenEngine(&engine) || !BeginPlayback(&engine)) return false;
  fake::FailPlaybackPreparation();
  for (std::uint64_t sequence = 0U; sequence < 9U; ++sequence) {
    if (!QueueFrame(&engine, sequence)) return false;
  }
  // 服务端尚未发 EOS；本地准备失败必须立即通知 application，不能等待下行结束。
  return Check(WaitForPlaybackDone(engine, 500ms) && engine.playback_failed(),
               "playback preparation failure was not published") &&
      Check(fake::RenderCallsSnapshot().empty(), "failed preparation still rendered PCM");
}

bool TestVoiceAudioPreRollOrder() {
  VoiceAudio voice;
  if (!OpenVoice(&voice) || !BeginVoiceListen(&voice, false)) return false;

  AudioEvent event{};
  for (std::int16_t id = 1; id <= 55; ++id) {
    CaptureFrame frame{};
    frame.pcm[0] = id;
    frame.timestamp_us = static_cast<std::uint64_t>(id) * 20000U;
    if (!Check(!PollInjected(&voice, frame, &event),
               "pre-roll history emitted PCM before speech admission"))
      return false;
  }
  CaptureFrame admitted{};
  admitted.pcm[0] = 56;
  admitted.timestamp_us = 56U * 20000U;
  admitted.vad_now = admitted.vad_started = true;
  if (!Check(PollInjected(&voice, admitted, &event) &&
                 event.kind == AudioEventKind::SpeechStart,
             "speech admission did not lead with SpeechStart"))
    return false;

  std::uint64_t previous_sequence = 0U;
  for (std::int16_t expected = 32; expected <= 56; ++expected) {
    if (!Check(voice.Poll(&event, 100ms) &&
                   event.kind == AudioEventKind::Pcm,
               "pre-roll PCM was missing") ||
        !Check(event.pcm[0] == expected,
               "pre-roll did not retain the newest contiguous 25 frames") ||
        !Check(event.timestamp_us ==
                   static_cast<std::uint64_t>(expected) * 20000U,
               "pre-roll timestamp changed") ||
        !Check(!event.end, "admission pre-roll acquired a false END"))
      return false;
    if (expected != 32 &&
        !Check(event.sequence == previous_sequence + 1U,
               "pre-roll capture sequence was not contiguous"))
      return false;
    previous_sequence = event.sequence;
  }
  voice.CancelInput();
  voice.Close();
  return true;
}

bool TestVoiceAudioFollowUpDropsOldEnd() {
  VoiceAudio voice;
  if (!OpenVoice(&voice) || !BeginVoiceListen(&voice, true)) return false;

  AudioEvent event{};
  for (std::int16_t id = 1; id <= 10; ++id) {
    CaptureFrame frame{};
    frame.pcm[0] = id;
    frame.timestamp_us = static_cast<std::uint64_t>(id) * 20000U;
    frame.near_voice = frame.vad_now = true;
    if (!Check(!PollInjected(&voice, frame, &event),
               "short follow-up was admitted before 400 ms"))
      return false;
  }
  CaptureFrame old_end{};
  old_end.pcm[0] = 11;
  old_end.timestamp_us = 11U * 20000U;
  old_end.vad_ended = true;
  if (!Check(!PollInjected(&voice, old_end, &event),
             "rejected short follow-up emitted an event"))
    return false;

  for (std::int16_t id = 100; id <= 119; ++id) {
    CaptureFrame frame{};
    frame.pcm[0] = id;
    frame.timestamp_us = static_cast<std::uint64_t>(id) * 20000U;
    frame.near_voice = frame.vad_now = true;
    const bool have_event = PollInjected(&voice, frame, &event);
    if (id != 119 &&
        !Check(!have_event, "follow-up was admitted before 20 new frames"))
      return false;
    if (id == 119 &&
        !Check(have_event && event.kind == AudioEventKind::SpeechStart,
               "new follow-up was not admitted"))
      return false;
  }
  for (std::int16_t expected = 100; expected <= 119; ++expected) {
    if (!Check(voice.Poll(&event, 100ms) &&
                   event.kind == AudioEventKind::Pcm,
               "new follow-up PCM was missing") ||
        !Check(event.pcm[0] == expected,
               "old short utterance contaminated new follow-up") ||
        !Check(!event.end, "old VAD END truncated the new follow-up"))
      return false;
  }
  voice.CancelInput();
  voice.Close();
  return true;
}

bool DriveConfirmedBarge(VoiceAudio* const voice, AudioEvent* const event) {
  std::int16_t id = 1;
  auto drive = [&](const unsigned count, const bool reference,
                   const bool near_voice) {
    for (unsigned index = 0U; index < count; ++index, ++id) {
      CaptureFrame frame{};
      frame.pcm[0] = id;
      frame.timestamp_us = static_cast<std::uint64_t>(id) * 20000U;
      frame.reference_active = reference;
      frame.near_voice = near_voice;
      frame.vad_now = near_voice;
      frame.voice_dbfs = -10.0F;
      if (PollInjected(voice, frame, event)) return false;
    }
    return true;
  };
  if (!drive(6U, true, true) || !drive(3U, false, true) ||
      !drive(3U, false, true))
    return false;
  for (unsigned index = 0U; index < 3U; ++index, ++id) {
    CaptureFrame frame{};
    frame.pcm[0] = id;
    frame.timestamp_us = static_cast<std::uint64_t>(id) * 20000U;
    frame.near_voice = frame.vad_now = true;
    frame.voice_dbfs = -10.0F;
    const bool have_event = PollInjected(voice, frame, event);
    if (index != 2U && have_event) return false;
    if (index == 2U)
      return have_event && event->kind == AudioEventKind::Barge;
  }
  return false;
}

bool TestVoiceAudioBargeAndDropLifecycle() {
  VoiceAudio voice;
  if (!OpenVoice(&voice)) return false;
  const auto pcm = Pcm24(480U, 1000);
  if (!BeginVoicePlayback(&voice, 7U, pcm)) return false;
  fake::BlockPlayback(fake::PlaybackBlock::kRender);
  for (std::uint32_t sequence = 1U; sequence < 9U; ++sequence) {
    if (!Check(voice.Play(7U, pcm.data(), pcm.size(), sequence, false,
                          false),
               "VoiceAudio could not fill the initial playback buffer"))
      return false;
  }
  if (!Check(fake::WaitForPlaybackBlocked(500ms),
             "playback did not enter the blocked render"))
    return false;

  AudioEvent event{};
  if (!Check(DriveConfirmedBarge(&voice, &event),
             "production barge detector did not confirm") ||
      !Check(event.generation == 7U,
             "Barge was not bound to the interrupted generation"))
    return false;

  // Application starts the superseding input from the queued PCM, then may
  // cancel it locally. A subsequent response must not fail merely because the
  // old playback thread is still completing its already-issued drop.
  voice.CancelInput();
  fake::BlockPlayback(fake::PlaybackBlock::kNone);
  const auto started = std::chrono::steady_clock::now();
  if (!BeginVoicePlayback(&voice, 8U, pcm)) return false;
  const auto elapsed = std::chrono::steady_clock::now() - started;
  if (!Check(elapsed < 250ms,
             "drop-to-next-playback lifecycle was not bounded"))
    return false;
  voice.StopPlayback();
  voice.Close();
  return true;
}

}  // namespace

int main(const int argc, char** const argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: %s <scenario>\n", argv[0]);
    return 2;
  }
  const std::string scenario = argv[1];
  bool passed = false;
  if (scenario == "sub-grace-jitter")
    passed = TestSubGraceJitter() && TestPlaybackXrunCounter();
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
  else if (scenario == "playback-owner-order") passed = TestPlaybackOwnerOrder();
  else if (scenario == "drop-blocked-render")
    passed = TestInterruptBlockedPlayback(fake::PlaybackBlock::kRender, false);
  else if (scenario == "drop-blocked-drain")
    passed = TestInterruptBlockedPlayback(fake::PlaybackBlock::kDrain, false);
  else if (scenario == "close-blocked-render")
    passed = TestInterruptBlockedPlayback(fake::PlaybackBlock::kRender, true);
  else if (scenario == "playback-prepare-failure") passed = TestPlaybackPrepareFailure();
  else if (scenario == "voice-preroll") passed = TestVoiceAudioPreRollOrder();
  else if (scenario == "voice-follow-up-boundary")
    passed = TestVoiceAudioFollowUpDropsOldEnd();
  else if (scenario == "voice-barge-lifecycle")
    passed = TestVoiceAudioBargeAndDropLifecycle();
  else {
    std::fprintf(stderr, "unknown scenario: %s\n", scenario.c_str());
    return 2;
  }
  if (passed) std::printf("audio-engine harness: %s passed\n", scenario.c_str());
  return passed ? 0 : 1;
}
