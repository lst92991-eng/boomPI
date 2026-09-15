// 运行真实采集/播放namespace、FFmpeg、3A适配与检测；仅声卡I/O及vendor C核心替换。
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <future>
#include <string>
#include <thread>
#include <vector>

#include "audio_hardware.h"
#include "audio_vendor.h"
#include "boompi/audio/audio_capture.h"
#include "boompi/audio/playback.h"
#include "boompi/audio/speech.h"

namespace boompi::test {
bool TestModuleFormat();
bool TestModuleDetection();
}  // namespace boompi::test

namespace {
using namespace std::chrono_literals;
using boompi::audio::CaptureFrame;
using boompi::audio::RawCaptureFrame;
namespace capture = boompi::audio_capture;
namespace playback = boompi::playback;
namespace speech = boompi::speech;
namespace fake = boompi::test::audio_hardware;
namespace vendor = boompi::test::audio_vendor;

bool Check(bool condition, const char* message) {
  if (!condition) {
    std::fprintf(stderr, "audio harness: %s\n", message);
  }
  return condition;
}
void CloseAudio() {
  playback::close();
  capture::close();
}
bool OpenAudio() {
  CloseAudio();
  fake::reset();
  vendor::reset();
  speech::reset();
  return Check(capture::open(), "capture open failed") &&
         Check(fake::capture_reads() == 0, "capture read preceded playback configuration") &&
         Check(playback::open(100) && capture::start(), "audio start failed") &&
         Check(fake::wait_for_capture_reads(1, 500ms), "capture did not reach ALSA read");
}
bool WaitForPlaybackDone(std::chrono::milliseconds timeout = 500ms) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (playback::status().state == playback::State::Playing &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(1ms);
  }
  return playback::status().state != playback::State::Playing;
}
RawCaptureFrame RawTone(int amplitude = 1000) {
  RawCaptureFrame frame{};
  for (std::size_t i = 0; i < boompi::audio::kCaptureFrameSamples; ++i) {
    const auto sample = static_cast<std::int16_t>(amplitude * std::sin(i * 0.13));
    frame.pcm[4 * i] = sample;
    frame.pcm[4 * i + 1] = sample;
  }
  return frame;
}
bool BeginPlayback(std::uint32_t generation = 1) {
  auto result = std::async(std::launch::async, [generation] {
    return playback::begin(generation);
  });
  for (unsigned i = 0; i < 10 && result.wait_for(0ms) != std::future_status::ready; ++i) {
    fake::push_capture({});
    std::this_thread::sleep_for(3ms);
  }
  if (!Check(result.wait_for(500ms) == std::future_status::ready && result.get(),
             "playback begin did not cross capture boundary")) {
    return false;
  }
  CaptureFrame ignored{};
  while (capture::read(&ignored, 0ms) == capture::ReadResult::Frame) {
  }
  return true;
}
std::vector<std::uint8_t> Pcm16(std::size_t samples, std::int16_t value) {
  std::vector<std::uint8_t> bytes(samples * 2);
  const auto bits = static_cast<std::uint16_t>(value);
  for (std::size_t i = 0; i < samples; ++i) {
    bytes[2 * i] = static_cast<std::uint8_t>(bits);
    bytes[2 * i + 1] = static_cast<std::uint8_t>(bits >> 8);
  }
  return bytes;
}
bool QueueFrame(std::uint32_t sequence, std::size_t samples = 320,
                std::uint32_t generation = 1) {
  const auto bytes = Pcm16(samples, static_cast<std::int16_t>(1000 + sequence * 100));
  return Check(playback::write(generation, bytes.data(), bytes.size(), sequence) ==
                   playback::WriteResult::Queued,
               "valid PCM did not enter playback");
}
bool PrimePlayback(std::uint32_t generation = 1, std::size_t previous_writes = 0) {
  if (!BeginPlayback(generation)) {
    return false;
  }
  for (std::uint32_t sequence = 0; sequence < 9; ++sequence) {
    if (!QueueFrame(sequence, 320, generation)) {
      return false;
    }
  }
  return Check(fake::wait_for_writes(previous_writes + 9, 500ms),
               "180ms prebuffer did not reach real resampling and ALSA writes");
}
bool StereoDuration(std::size_t input_samples) {
  const auto pcm = fake::written_samples();
  if (!Check(pcm.size() == input_samples * 3 * 2,
             "playback/flush lost valid samples or appended padding")) {
    return false;
  }
  for (std::size_t i = 0; i < pcm.size(); i += 2) {
    if (!Check(pcm[i] == pcm[i + 1], "left/right samples differ")) {
      return false;
    }
  }
  const auto operations = fake::operations();
  const auto drain = std::find(operations.begin(), operations.end(), "playback.drain");
  return Check(drain != operations.end() &&
                   std::find(drain, operations.end(), "playback.write") == operations.end(),
               "filter tail was written after ALSA drain");
}

bool TestSubGraceJitter() {
  if (!OpenAudio() || !PrimePlayback()) {
    return false;
  }
  for (std::uint32_t sequence = 9; sequence < 12; ++sequence) {
    std::this_thread::sleep_for(5ms);
    if (!QueueFrame(sequence) || !fake::wait_for_writes(sequence + 1, 100ms)) {
      return Check(false, "sub-30ms jitter forced rebuffering");
    }
  }
  return playback::finish(1) && WaitForPlaybackDone() && StereoDuration(12 * 320);
}
bool TestRebufferAfterConfirmedGap() {
  if (!OpenAudio() || !PrimePlayback()) {
    return false;
  }
  std::this_thread::sleep_for(65ms);
  if (!QueueFrame(9)) {
    return false;
  }
  std::this_thread::sleep_for(15ms);
  if (!Check(fake::write_count() == 9, "confirmed gap resumed on a single packet") ||
      !QueueFrame(10) || !fake::wait_for_writes(11, 200ms)) {
    return false;
  }
  return playback::finish(1) && WaitForPlaybackDone() && StereoDuration(11 * 320);
}
bool TestEndPlaybackShortTail() {
  if (!OpenAudio() || !PrimePlayback()) {
    return false;
  }
  std::this_thread::sleep_for(65ms);
  if (!QueueFrame(9, 240)) {
    return false;
  }
  const auto extra = Pcm16(320, 99);
  if (!Check(
          playback::write(1, extra.data(), extra.size(), 10) == playback::WriteResult::Ending,
          "PCM after short tail was accepted") ||
      !Check(fake::write_count() == 9, "short tail started before END") ||
      !playback::finish(1) || !WaitForPlaybackDone()) {
    return false;
  }
  return StereoDuration(9 * 320 + 240);
}
bool TestSingleFrameReply(std::size_t samples, std::int16_t value) {
  if (!OpenAudio() || !BeginPlayback()) {
    return false;
  }
  const auto input = Pcm16(samples, 0);
  auto bytes = input;
  const auto bits = static_cast<std::uint16_t>(value);
  bytes[2 * (samples - 1)] = static_cast<std::uint8_t>(bits);
  bytes[2 * (samples - 1) + 1] = static_cast<std::uint8_t>(bits >> 8);
  if (playback::write(1, bytes.data(), bytes.size(), 0) != playback::WriteResult::Queued ||
      !playback::finish(1) || !WaitForPlaybackDone() || !StereoDuration(samples)) {
    return false;
  }
  const auto pcm = fake::written_samples();
  const auto peak = *std::max_element(pcm.begin(), pcm.end(), [](auto left, auto right) {
    return std::abs(static_cast<int>(left)) < std::abs(static_cast<int>(right));
  });
  return Check(std::abs(static_cast<int>(peak)) > 16000 && (peak < 0) == (value < 0),
               "one-sample/last-sample reply lost sign or filter tail");
}
bool TestBoundedClose() {
  if (!OpenAudio()) {
    return false;
  }
  const auto start = std::chrono::steady_clock::now();
  CloseAudio();
  return Check(
      std::chrono::steady_clock::now() - start < 200ms && fake::capture_interrupts() != 0,
      "close failed to interrupt blocked capture");
}
bool TestBoundedCaptureWait() {
  if (!OpenAudio()) {
    return false;
  }
  CaptureFrame frame{};
  const auto start = std::chrono::steady_clock::now();
  const auto result = capture::read(&frame, 40ms);
  const auto elapsed = std::chrono::steady_clock::now() - start;
  if (!Check(result == capture::ReadResult::Timeout && elapsed >= 30ms && elapsed < 500ms,
             "capture timeout was not bounded")) {
    return false;
  }
  for (unsigned i = 0; i < 5; ++i) {
    fake::push_capture(RawTone());
  }
  return Check(fake::wait_for_capture_reads(6, 500ms) &&
                   capture::read(&frame, 100ms) == capture::ReadResult::Frame &&
                   frame.sequence == 4 && frame.discontinuity && frame.actor_overrun,
               "capture overrun concealed a missing frame");
}
bool TestBoundedCaptureCommand() {
  if (!OpenAudio()) {
    return false;
  }
  const auto start = std::chrono::steady_clock::now();
  const bool began = playback::begin(1);
  const auto elapsed = std::chrono::steady_clock::now() - start;
  return Check(!began && elapsed >= 80ms && elapsed < 250ms,
               "capture command did not fail in 100ms without a frame boundary");
}
bool TestResetPreservesQueuedPcm() {
  if (!OpenAudio()) {
    return false;
  }
  vendor::wake_result = 2;
  for (unsigned i = 0; i < 3; ++i) {
    fake::push_capture(RawTone(1000 + static_cast<int>(i) * 1000));
  }
  if (!fake::wait_for_capture_reads(4, 500ms)) {
    return false;
  }
  auto reset = std::async(std::launch::async, [] {
    return capture::reset_listener();
  });
  std::this_thread::sleep_for(10ms);
  fake::push_capture(RawTone(4000));
  if (!Check(reset.wait_for(500ms) == std::future_status::ready && reset.get(),
             "listener reset did not cross raw capture boundary")) {
    return false;
  }
  bool have_pcm = false;
  std::uint64_t previous_timestamp = 0;
  for (std::uint64_t i = 0; i < 4; ++i) {
    CaptureFrame frame{};
    if (!Check(capture::read(&frame, 100ms) == capture::ReadResult::Frame &&
                   frame.sequence == i && frame.timestamp_us >= previous_timestamp &&
                   !frame.discontinuity && !frame.wake && !frame.vad_now &&
                   !frame.vad_started && !frame.vad_ended && !frame.near_voice,
               "reset erased PCM timeline or kept old detection flags")) {
      return false;
    }
    previous_timestamp = frame.timestamp_us;
    have_pcm |= std::any_of(frame.pcm.begin(), frame.pcm.end(), [](auto value) {
      return value != 0;
    });
  }
  return Check(have_pcm && vendor::dsp_calls == 5, "capture bypassed real conversion/3A");
}
bool TestQueueResults() {
  if (!OpenAudio()) {
    return false;
  }
  const auto frame = Pcm16(320, 1000);
  if (!Check(playback::write(1, nullptr, 0, 0) == playback::WriteResult::InvalidArgument &&
                 playback::write(1, frame.data(), frame.size(), 0) ==
                     playback::WriteResult::NotActive,
             "invalid/inactive results were conflated") ||
      !BeginPlayback()) {
    return false;
  }
  const auto oversized = Pcm16(321, 1);
  if (!Check(playback::write(1, oversized.data(), oversized.size(), 0) ==
                     playback::WriteResult::InvalidArgument &&
                 playback::write(1, frame.data(), frame.size() - 1, 0) ==
                     playback::WriteResult::InvalidArgument &&
                 playback::write(1, frame.data(), frame.size(), 1) ==
                     playback::WriteResult::Discontinuous &&
                 playback::write(2, frame.data(), frame.size(), 0) ==
                     playback::WriteResult::StaleGeneration &&
                 playback::write(1, frame.data(), frame.size(), 0) ==
                     playback::WriteResult::Queued &&
                 playback::write(1, frame.data(), frame.size(), 2) ==
                     playback::WriteResult::Discontinuous &&
                 playback::error().empty(),
             "length/sequence/generation checks lost distinct outcomes")) {
    return false;
  }
  playback::cancel();
  return WaitForPlaybackDone();
}
bool TestQueueCapacity() {
  if (!OpenAudio() || !BeginPlayback()) {
    return false;
  }
  fake::block_playback(fake::PlaybackBlock::Write);
  for (unsigned i = 0; i < 9; ++i) {
    if (!QueueFrame(i)) {
      return false;
    }
  }
  if (!fake::wait_for_playback_blocked(500ms)) {
    return false;
  }
  for (unsigned i = 9; i <= 75; ++i) {
    if (!QueueFrame(i)) {
      return false;
    }
  }
  const auto extra = Pcm16(320, 999);
  const bool rejected =
      playback::write(1, extra.data(), extra.size(), 76) == playback::WriteResult::Full;
  playback::cancel();
  return Check(
      rejected && WaitForPlaybackDone() && playback::status().state == playback::State::Idle,
      "full queue overwrote PCM or could not cancel");
}
bool TestOpenClearsError() {
  CloseAudio();
  fake::reset();
  vendor::reset();
  fake::fail_capture_open(true);
  if (!Check(!capture::open() && !capture::error().empty(),
             "ALSA open failure did not produce a diagnostic")) {
    return false;
  }
  fake::fail_capture_open(false);
  return Check(capture::open() && capture::error().empty(),
               "successful reopen kept the previous ALSA error");
}
bool TestPlaybackPrepareFailure(bool cancel_race = false);
bool TestPlaybackClearsError() {
  if (!TestPlaybackPrepareFailure(true)) {
    return false;
  }
  // 真实设备失败锁存；只有close/open明确重建资源后才允许重新播放。
  if (!OpenAudio() || !BeginPlayback()) {
    return false;
  }
  return Check(playback::error().empty() && QueueFrame(0) && playback::finish(1) &&
                   WaitForPlaybackDone() &&
                   playback::status().state == playback::State::Drained,
               "successful playback retained the old prepare failure");
}
bool TestPlaybackOwnerOrder() {
  if (!OpenAudio() || !PrimePlayback() || !playback::finish(1) || !WaitForPlaybackDone()) {
    return false;
  }
  const auto count = fake::write_count();
  return PrimePlayback(2, count) && playback::finish(2) && WaitForPlaybackDone() &&
         Check(fake::owner_order_valid() && fake::drain_count() == 2,
               "capture/playback operations crossed thread owners");
}
bool TestInterruptBlockedPlayback(fake::PlaybackBlock stage, bool close) {
  if (!OpenAudio() || !BeginPlayback()) {
    return false;
  }
  fake::block_playback(stage);
  if (!QueueFrame(0) || !playback::finish(1) || !fake::wait_for_playback_blocked(500ms)) {
    return false;
  }
  const auto writes = fake::write_count();
  const auto start = std::chrono::steady_clock::now();
  if (close) {
    CloseAudio();
  } else {
    playback::cancel();
  }
  if (!Check(WaitForPlaybackDone(200ms) && std::chrono::steady_clock::now() - start < 250ms &&
                 playback::status().state == playback::State::Idle &&
                 fake::write_count() == writes && fake::owner_order_valid(),
             "cancel/close leaked an old write or did not interrupt blocked I/O")) {
    return false;
  }
  return true;
}
bool TestPlaybackPrepareFailure(bool cancel_race) {
  if (!OpenAudio() || !BeginPlayback()) {
    return false;
  }
  fake::fail_playback_preparation();
  if (cancel_race) {
    fake::block_playback(fake::PlaybackBlock::Prepare);
  }
  for (unsigned i = 0; i < 9; ++i) {
    if (!QueueFrame(i)) {
      return false;
    }
  }
  if (cancel_race) {
    if (!Check(fake::wait_for_playback_blocked(500ms), "prepare race never entered hardware")) {
      return false;
    }
    // prepare期间播放线程持状态锁。取消先等该锁，再与失败收尾竞争，不能抹掉真实错误。
    auto cancel = std::async(std::launch::async, [] {
      playback::cancel();
    });
    std::this_thread::sleep_for(5ms);
    fake::block_playback(fake::PlaybackBlock::None);
    if (!Check(cancel.wait_for(200ms) == std::future_status::ready,
               "cancel stayed blocked after failed prepare")) {
      return false;
    }
    cancel.get();
  }
  if (!Check(WaitForPlaybackDone() && playback::status().state == playback::State::Failed &&
                 !playback::error().empty() && fake::write_count() == 0,
             "prepare failure waited for END, wrote PCM, or disappeared after cancel")) {
    return false;
  }
  const auto error = playback::error();
  playback::cancel();
  return Check(!playback::begin(2) && playback::status().state == playback::State::Failed &&
                   playback::error() == error,
               "cancel or a new generation cleared the latched device failure");
}

CaptureFrame SpeechFrame(std::uint64_t id, bool voice = false) {
  CaptureFrame frame{};
  frame.pcm[0] = static_cast<std::int16_t>(id);
  frame.sequence = id;
  frame.timestamp_us = id * 20000;
  frame.vad_now = frame.near_voice = voice;
  frame.voice_dbfs = voice ? -10 : -120;
  return frame;
}
bool TestSpeechPreRoll() {
  speech::listen(speech::ListenMode::Wake);
  for (unsigned id = 1; id <= 55; ++id) {
    auto frame = SpeechFrame(id);
    if (!Check(speech::update(frame, false).decision == speech::Decision::None,
               "pre-roll emitted PCM before admission")) {
      return false;
    }
  }
  auto admitted = SpeechFrame(56, true);
  admitted.vad_started = true;
  const auto batch = speech::update(admitted, false);
  if (!Check(batch.decision == speech::Decision::Start && batch.count == 25 && !batch.end,
             "speech admission lost its 500ms pre-roll")) {
    return false;
  }
  for (std::size_t i = 0; i < batch.count; ++i) {
    if (!Check(batch.frames[i]->pcm[0] == static_cast<int>(32 + i) &&
                   batch.frames[i]->sequence == 32 + i &&
                   batch.frames[i]->timestamp_us == (32 + i) * 20000,
               "pre-roll duplicated/reordered current PCM or metadata")) {
      return false;
    }
  }
  auto tail = SpeechFrame(57);
  tail.vad_ended = true;
  const auto ended = speech::update(tail, false);
  return Check(ended.decision == speech::Decision::Pcm && ended.count == 1 && ended.end &&
                   ended.frames[0]->pcm[0] == 57,
               "real-time END omitted the last PCM frame");
}
bool TestSpeechFollowUp() {
  speech::listen(speech::ListenMode::FollowUp);
  for (unsigned id = 1; id <= 10; ++id) {
    auto frame = SpeechFrame(id, true);
    if (speech::update(frame, false).decision != speech::Decision::None) {
      return Check(false, "short follow-up was admitted before 400ms");
    }
  }
  auto old_end = SpeechFrame(11);
  old_end.vad_ended = true;
  speech::update(old_end, false);
  for (unsigned id = 12; id <= 31; ++id) {
    auto frame = SpeechFrame(id, true);
    const auto result = speech::update(frame, false);
    if (id < 31 && result.decision != speech::Decision::None) {
      return Check(false, "follow-up reused old admission count");
    }
    if (id == 31) {
      if (!Check(
              result.decision == speech::Decision::Start && result.count == 20 && !result.end,
              "new follow-up inherited old END")) {
        return false;
      }
      for (std::size_t i = 0; i < result.count; ++i) {
        if (!Check(result.frames[i]->pcm[0] == static_cast<int>(12 + i),
                   "old rejected follow-up polluted pre-roll")) {
          return false;
        }
      }
    }
  }
  return true;
}
bool TestSpeechBarge() {
  speech::reset();
  // 探针走120ms候选、60ms低参考、60ms尾音、60ms确认，不能VAD一命中就取消。
  for (unsigned id = 1; id <= 15; ++id) {
    auto frame = SpeechFrame(id, true);
    frame.reference_active = id <= 6;
    const auto result = speech::update(frame, true);
    if (id < 15 && !Check(result.decision != speech::Decision::Barge,
                          "barge skipped acoustic confirmation")) {
      return false;
    }
    if (id >= 6 && id < 15 &&
        !Check(result.playback_scale == 0, "barge probe did not request temporary mute")) {
      return false;
    }
    if (id == 15) {
      if (!Check(result.decision == speech::Decision::Barge && result.count == 15 &&
                     result.playback_scale == 1,
                 "confirmed barge lost buffered near speech")) {
        return false;
      }
      for (std::size_t i = 0; i < result.count; ++i) {
        if (!Check(result.frames[i]->pcm[0] == static_cast<int>(i + 1),
                   "barge repeated/reordered probe PCM")) {
          return false;
        }
      }
    }
  }
  auto next = SpeechFrame(16, true);
  const auto continuing = speech::update(next, false);
  if (!Check(continuing.decision == speech::Decision::Pcm && continuing.count == 1 &&
                 continuing.frames[0]->pcm[0] == 16,
             "confirmed barge did not continue as the same utterance")) {
    return false;
  }
  // 另走真实播放取消→新轮，确认重采样尾音也不会复活。
  if (!OpenAudio() || !BeginPlayback()) {
    return false;
  }
  fake::block_playback(fake::PlaybackBlock::Write);
  if (!QueueFrame(0) || !playback::finish(1) || !fake::wait_for_playback_blocked(500ms)) {
    return false;
  }
  playback::cancel();
  if (!WaitForPlaybackDone()) {
    return false;
  }
  fake::block_playback(fake::PlaybackBlock::None);
  if (!BeginPlayback(2)) {
    return false;
  }
  const auto silence = Pcm16(320, 0);
  if (playback::write(2, silence.data(), silence.size(), 0) != playback::WriteResult::Queued ||
      !playback::finish(2) || !WaitForPlaybackDone()) {
    return false;
  }
  const auto output = fake::written_samples();
  return Check(std::all_of(output.begin(), output.end(),
                           [](auto value) {
                             return value == 0;
                           }),
               "cancelled filter tail appeared in next reply");
}
}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: %s <scenario>\n", argv[0]);
    return 2;
  }
  const std::string scenario = argv[1];
  bool passed = false;
  try {
    if (scenario == "sub-grace-jitter") {
      passed = TestSubGraceJitter();
    } else if (scenario == "confirmed-gap") {
      passed = TestRebufferAfterConfirmedGap();
    } else if (scenario == "short-tail") {
      passed = TestEndPlaybackShortTail() && TestSingleFrameReply(1, -32768) &&
               TestSingleFrameReply(320, 32767);
    } else if (scenario == "bounded-close") {
      passed = TestBoundedClose();
    } else if (scenario == "bounded-capture") {
      passed = TestBoundedCaptureWait();
    } else if (scenario == "bounded-command") {
      passed = TestBoundedCaptureCommand();
    } else if (scenario == "reset-preserves-pcm") {
      passed = TestResetPreservesQueuedPcm();
    } else if (scenario == "queue-results") {
      passed = TestQueueResults() && TestQueueCapacity();
    } else if (scenario == "open-clears-error") {
      passed = TestOpenClearsError();
    } else if (scenario == "playback-clears-error") {
      passed = TestPlaybackClearsError();
    } else if (scenario == "playback-owner-order") {
      passed = TestPlaybackOwnerOrder();
    } else if (scenario == "drop-blocked-render") {
      passed = TestInterruptBlockedPlayback(fake::PlaybackBlock::Write, false);
    } else if (scenario == "drop-blocked-drain") {
      passed = TestInterruptBlockedPlayback(fake::PlaybackBlock::Drain, false);
    } else if (scenario == "close-blocked-render") {
      passed = TestInterruptBlockedPlayback(fake::PlaybackBlock::Write, true);
    } else if (scenario == "playback-prepare-failure") {
      passed = TestPlaybackPrepareFailure();
    } else if (scenario == "voice-preroll") {
      passed = TestSpeechPreRoll();
    } else if (scenario == "voice-follow-up-boundary") {
      passed = TestSpeechFollowUp();
    } else if (scenario == "voice-barge-lifecycle") {
      passed = TestSpeechBarge();
    } else if (scenario == "modules-format") {
      passed = boompi::test::TestModuleFormat();
    } else if (scenario == "modules-detection") {
      passed = boompi::test::TestModuleDetection();
    } else {
      std::fprintf(stderr, "unknown scenario: %s\n", argv[1]);
    }
  } catch (const std::exception& error) {
    std::fprintf(stderr, "audio harness: %s\n", error.what());
  }
  CloseAudio();
  std::printf("audio harness: %s %s\n", argv[1], passed ? "passed" : "FAILED");
  return passed ? 0 : 1;
}
