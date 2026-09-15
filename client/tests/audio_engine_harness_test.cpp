/**
 * @file audio_engine_harness_test.cpp
 * @brief 用可控 I/O 驱动真实 AudioTasks/VoiceAudio，验证线程时序和语句生命周期。
 *
 * 从 main 的 scenario 分发表选择测试：准备后端 → 推进采集帧/下行包 → 等待明确的
 * 处理位置 → 核对输出和截止时间。实际 ALSA 由 tests/support/audio_pipeline 替代；
 * pipeline-format/detection 则调用 audio_pipeline_test.cpp 直接验证生产转换与检测。
 */
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <future>
#include <string>
#include <thread>
#include <vector>

#include "audio_pipeline.h"
#include "boompi/audio/audio_tasks.h"
#include "boompi/audio/voice_audio.h"

namespace boompi::test {
bool TestPipelineFormat();
bool TestPipelineDetection();
}  // namespace boompi::test

namespace {

using namespace std::chrono_literals;
using boompi::audio::AudioEvent;
using boompi::audio::AudioEventKind;
using boompi::audio::AudioTasks;
using boompi::audio::CaptureFrame;
using boompi::audio::CaptureResult;
using boompi::audio::ListenMode;
using boompi::audio::QueueTtsResult;
using boompi::audio::VoiceAudio;
namespace fake = boompi::test::audio_pipeline;

/** @brief 将单个契约失败打印到标准错误，返回布尔值供测试链提前退出。 */
bool Check(const bool condition, const char* const message) {
  if (condition) {
    return true;
  }
  std::fprintf(stderr, "audio-engine harness: %s\n", message);
  return false;
}

/** @brief 有截止时间地等播放线程完成，避免失败场景让测试进程永久挂起。 */
bool WaitForPlaybackDone(const AudioTasks& engine, const std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (!engine.IsPlaybackDone() && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(1ms);
  }
  return engine.IsPlaybackDone();
}

/** @brief 清空脚本并启动引擎，等采集线程进入第一次 read 后才向它发送控制命令。 */
bool OpenEngine(AudioTasks* const engine) {
  fake::Reset();
  return Check(engine->Start(), "Open failed") &&
         Check(fake::WaitForCaptureReads(1U, 500ms),
               "capture actor did not enter its blocking read");
}

/** @brief 在辅助线程发起 BeginPlayback，同时注入采集帧，让命令跨过生产帧边界。 */
bool BeginPlayback(AudioTasks* const engine) {
  std::promise<void> entered;
  auto entered_future = entered.get_future();
  auto begin = std::async(std::launch::async, [engine, entered = std::move(entered)]() mutable {
    entered.set_value();
    return engine->BeginPlayback();
  });
  entered_future.wait();
  // 真实后端每 20 ms 从 ALSA 返回一次；假采集帧提供执行命令所需的同类帧边界。
  for (std::size_t attempt = 0U;
       attempt < 10U && begin.wait_for(0ms) != std::future_status::ready; ++attempt) {
    fake::PushCapture(CaptureFrame{});
    std::this_thread::sleep_for(5ms);
  }
  if (!Check(begin.wait_for(500ms) == std::future_status::ready,
             "BeginPlayback did not complete at a capture boundary")) {
    return false;
  }
  return Check(begin.get(), "BeginPlayback failed");
}

/** @brief 生成 S16_LE 字节包；样本值携带帧编号，方便检测乱序、截断和符号位错误。 */
std::vector<std::uint8_t> Pcm16(const std::size_t samples, const std::int16_t value) {
  std::vector<std::uint8_t> bytes(samples * 2U);
  const auto bits = static_cast<std::uint16_t>(value);
  for (std::size_t i = 0U; i < samples; ++i) {
    bytes[2U * i] = static_cast<std::uint8_t>(bits & 0xffU);
    bytes[2U * i + 1U] = static_cast<std::uint8_t>(bits >> 8U);
  }
  return bytes;
}

/** @brief 启动包含语句整理的真实 VoiceAudio，并等待其采集线程进入 I/O。 */
bool OpenVoice(VoiceAudio* const voice) {
  fake::Reset();
  return Check(voice->Open(60U), "VoiceAudio Open failed") &&
         Check(fake::WaitForCaptureReads(1U, 500ms), "VoiceAudio capture actor did not start");
}

/** @brief 给 Listen 的检测器复位请求提供采集边界，避免假 read 无限阻塞控制命令。 */
bool BeginVoiceListen(VoiceAudio* const voice, const ListenMode mode) {
  auto listen = std::async(std::launch::async, [voice, mode] {
    return voice->Listen(mode);
  });
  std::size_t injected = 0;
  for (; injected < 10U && listen.wait_for(0ms) != std::future_status::ready; ++injected) {
    fake::PushCapture(CaptureFrame{});
    std::this_thread::sleep_for(5ms);
  }
  if (!Check(listen.wait_for(500ms) == std::future_status::ready,
             "VoiceAudio Listen did not cross a capture boundary") ||
      !Check(listen.get(), "VoiceAudio Listen failed")) {
    return false;
  }
  // 控制命令保留已采集 PCM。先消费这些启动用空帧，后续每次注入才对应当前测试帧。
  std::vector<AudioEvent> events;
  for (std::size_t i = 0; i < injected; ++i) {
    voice->ProcessEvents(events, 0ms);
  }
  return events.empty();
}

/** @brief 注入采集帧驱动首包的播放准备；后续下行包无需重复开始同一轮播放。 */
bool BeginVoicePlayback(VoiceAudio* const voice, const std::uint32_t generation,
                        const std::vector<std::uint8_t>& pcm) {
  auto play = std::async(std::launch::async, [voice, generation, &pcm] {
    return voice->Play(generation, pcm.data(), pcm.size(), 0U, true, false);
  });
  std::size_t injected = 0;
  for (; injected < 20U && play.wait_for(0ms) != std::future_status::ready; ++injected) {
    fake::PushCapture(CaptureFrame{});
    std::this_thread::sleep_for(5ms);
  }
  if (!Check(play.wait_for(500ms) == std::future_status::ready,
             "VoiceAudio Play(start) did not complete") ||
      !Check(play.get(), "VoiceAudio Play(start) failed")) {
    return false;
  }
  std::vector<AudioEvent> events;
  for (std::size_t i = 0; i < injected; ++i) {
    voice->ProcessEvents(events, 0ms);
  }
  return events.empty();
}

/** @brief 注入一帧并直接取得整批事件；空批次表示尚未准入。 */
void ProcessInjected(VoiceAudio& voice, CaptureFrame frame, std::vector<AudioEvent>& events) {
  fake::PushCapture(frame);
  voice.ProcessEvents(events, 100ms);
}

/** @brief 构造带编号的测试帧并要求完整入队，短帧场景通过 samples 显式指定长度。 */
bool QueueFrame(AudioTasks* const engine, const std::uint64_t sequence,
                const std::size_t samples = 320U) {
  const auto bytes = Pcm16(samples, static_cast<std::int16_t>(sequence + 1U));
  return Check(
      engine->QueueReplyFrame(bytes.data(), bytes.size(), sequence) == QueueTtsResult::Queued,
      "QueueReplyFrame failed");
}

/** @brief 填入九帧起播缓存并等后端记录九次渲染，随后在队列尾部制造到包间隙。 */
bool PrimePlayback(AudioTasks* const engine) {
  if (!BeginPlayback(engine)) {
    return false;
  }
  for (std::uint64_t sequence = 0U; sequence < 9U; ++sequence) {
    if (!QueueFrame(engine, sequence)) {
      return false;
    }
  }
  return Check(fake::WaitForRenderCalls(9U, 500ms), "initial 180 ms buffer was not rendered");
}

/** @brief 制造小于 30 ms 的到包抖动，验证宽限期内不会反复退回双帧蓄水。 */
bool TestSubGraceJitter() {
  AudioTasks engine;
  if (!OpenEngine(&engine) || !PrimePlayback(&engine)) {
    return false;
  }

  for (std::uint64_t sequence = 9U; sequence < 12U; ++sequence) {
    std::this_thread::sleep_for(5ms);
    if (!QueueFrame(&engine, sequence) ||
        !Check(fake::WaitForRenderCalls(static_cast<std::size_t>(sequence + 1U), 100ms),
               "a sub-30 ms gap incorrectly required two packets")) {
      return false;
    }
  }

  if (!Check(engine.EndPlayback(), "EndPlayback failed") ||
      !Check(WaitForPlaybackDone(engine, 500ms), "playback did not finish")) {
    return false;
  }
  return Check(fake::RenderCallsSnapshot().size() == 12U,
               "continuous playback rendered an unexpected frame count");
}

/** @brief 制造确定欠载后只补一帧应继续等待，第二帧到达才恢复播放。 */
bool TestRebufferAfterConfirmedGap() {
  AudioTasks engine;
  if (!OpenEngine(&engine) || !PrimePlayback(&engine)) {
    return false;
  }

  std::this_thread::sleep_for(60ms);
  if (!QueueFrame(&engine, 9U)) {
    return false;
  }
  std::this_thread::sleep_for(15ms);
  if (!Check(fake::RenderCallsSnapshot().size() == 9U,
             "confirmed underrun resumed from only one 20 ms packet")) {
    return false;
  }
  if (!QueueFrame(&engine, 10U) || !Check(fake::WaitForRenderCalls(11U, 200ms),
                                          "two buffered packets did not resume playback")) {
    return false;
  }

  return Check(engine.EndPlayback(), "EndPlayback failed") &&
         Check(WaitForPlaybackDone(engine, 500ms), "playback did not finish");
}

/** @brief 欠载期间提交短尾：拒绝后续 PCM，收到 END 后允许不足蓄水门限的尾包播放。 */
bool TestEndPlaybackShortTail() {
  AudioTasks engine;
  if (!OpenEngine(&engine) || !PrimePlayback(&engine)) {
    return false;
  }

  std::this_thread::sleep_for(60ms);
  if (!QueueFrame(&engine, 9U, 240U)) {
    return false;
  }
  const auto extra = Pcm16(320U, 99);
  if (!Check(engine.QueueReplyFrame(extra.data(), extra.size(), 10U) == QueueTtsResult::Ending,
             "PCM after a short tail was accepted") ||
      !Check(engine.LastError().empty(), "short-tail rejection polluted last_error") ||
      !Check(fake::RenderCallsSnapshot().size() == 9U,
             "a short tail rendered before EndPlayback") ||
      !Check(engine.EndPlayback(), "EndPlayback rejected a short tail") ||
      !Check(WaitForPlaybackDone(engine, 500ms),
             "short tail was stranded behind the rebuffer threshold")) {
    return false;
  }
  const auto calls = fake::RenderCallsSnapshot();
  return Check(calls.size() == 10U, "short tail was not rendered") &&
         Check(calls.back().pcm.size() == 240U,
               "short tail size changed before reaching the backend") &&
         Check(calls.back().pcm == std::vector<std::int16_t>(240U, 10),
               "rejected PCM changed the accepted short tail");
}

/** @brief 单包回复通过 END 绕过起播蓄水，且完整保留样本长度和正负满量程值。 */
bool TestSingleFrameReply(std::size_t samples, std::int16_t value) {
  AudioTasks engine;
  if (!OpenEngine(&engine) || !BeginPlayback(&engine)) {
    return false;
  }
  const auto bytes = Pcm16(samples, value);
  if (!Check(engine.QueueReplyFrame(bytes.data(), bytes.size(), 0U) == QueueTtsResult::Queued,
             "single-frame reply was rejected") ||
      !Check(engine.EndPlayback(), "single-frame EndPlayback failed") ||
      !Check(WaitForPlaybackDone(engine, 500ms), "single-frame reply did not finish")) {
    return false;
  }
  const auto calls = fake::RenderCallsSnapshot();
  return Check(calls.size() == 1U, "one packet did not produce one render") &&
         Check(calls.front().pcm == std::vector<std::int16_t>(samples, value),
               "single-frame sample count or signed PCM changed");
}

/** @brief 采集 read 阻塞时 Close 仍须通过 InterruptCapture 在有限时间内结束。 */
bool TestBoundedClose() {
  AudioTasks engine;
  if (!OpenEngine(&engine)) {
    return false;
  }

  const auto started = std::chrono::steady_clock::now();
  engine.Stop();
  const auto elapsed = std::chrono::steady_clock::now() - started;
  return Check(elapsed < 200ms, "Close waited on a blocked capture read") &&
         Check(fake::WaitForCaptureInterrupts(1U, 20ms), "Close did not call InterruptCapture");
}

/** @brief 无帧时消费者应得到 Timeout，而不是永久等待或把空队列判成设备故障。 */
bool TestBoundedCaptureWait() {
  AudioTasks engine;
  if (!OpenEngine(&engine)) {
    return false;
  }

  CaptureFrame frame{};
  const auto started = std::chrono::steady_clock::now();
  const CaptureResult result = engine.ReadProcessedFrame(&frame, 40ms);
  const auto elapsed = std::chrono::steady_clock::now() - started;
  return Check(result == CaptureResult::Timeout,
               "blocked capture was not reported as a timeout") &&
         Check(elapsed >= 30ms && elapsed < 500ms,
               "capture timeout was outside its bounded window");
}

/** @brief 不提供采集边界，验证 BeginPlayback 的控制等待会按上限失败。 */
bool TestBoundedCaptureCommand() {
  AudioTasks engine;
  if (!OpenEngine(&engine)) {
    return false;
  }

  const auto started = std::chrono::steady_clock::now();
  const bool began = engine.BeginPlayback();
  const auto elapsed = std::chrono::steady_clock::now() - started;
  return Check(!began, "capture-bound command unexpectedly succeeded") &&
         Check(elapsed >= 80ms && elapsed < 250ms,
               "capture-bound command did not fail in its bounded window");
}

/**
 * @brief 预先积压带检测标志的帧，再跨采集边界复位监听。
 * 已采集 PCM/序号/时间/电平/参考/断点必须保留，旧唤醒和 VAD 决策必须清除。
 */
bool TestResetPreservesQueuedPcm() {
  AudioTasks engine;
  if (!OpenEngine(&engine)) {
    return false;
  }

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
             "capture actor did not block after the queued frames")) {
    return false;
  }

  auto reset = std::async(std::launch::async, [&engine] {
    return engine.ResetListener();
  });
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
      !Check(reset.get(), "ResetListener failed")) {
    return false;
  }

  for (std::size_t index = 0U; index < 4U; ++index) {
    CaptureFrame actual{};
    if (!Check(engine.ReadProcessedFrame(&actual, 100ms) == CaptureResult::Frame,
               "queued PCM disappeared at reset") ||
        !Check(actual.sequence == index, "capture sequence changed at reset") ||
        !Check(actual.pcm[0] == static_cast<std::int16_t>(101U + index),
               "queued PCM changed at reset") ||
        !Check(actual.timestamp_us == 1000000U + index * 20000U,
               "capture timestamp changed at reset") ||
        !Check(actual.input_dbfs == -20.0F - static_cast<float>(index),
               "dBFS evidence changed at reset") ||
        !Check(actual.reference_active, "reference evidence changed at reset") ||
        !Check(!actual.wake && !actual.vad_now && !actual.vad_started && !actual.vad_ended &&
                   !actual.near_voice,
               "an old listener decision crossed the reset boundary")) {
      return false;
    }
    if (index == 0U &&
        !Check(actual.discontinuity, "capture discontinuity evidence changed at reset")) {
      return false;
    }
  }
  return true;
}

/** @brief 非法长度、未激活和序号空洞各有独立返回值，不污染设备错误状态。 */
bool TestQueueResultsDoNotPolluteLastError() {
  AudioTasks engine;
  if (!OpenEngine(&engine)) {
    return false;
  }
  const auto frame = Pcm16(320U, 1);
  if (!Check(engine.QueueReplyFrame(nullptr, 0U, 0U) == QueueTtsResult::InvalidArgument,
             "invalid PCM did not have a distinct queue result") ||
      !Check(
          engine.QueueReplyFrame(frame.data(), frame.size(), 0U) == QueueTtsResult::NotActive,
          "inactive playback did not have a distinct queue result") ||
      !Check(engine.LastError().empty(), "transient queue rejection polluted last_error") ||
      !BeginPlayback(&engine)) {
    return false;
  }

  const auto oversized = Pcm16(boompi::audio::kTtsFrameSamples + 1U, 1);
  if (!Check(engine.QueueReplyFrame(oversized.data(), oversized.size(), 0U) ==
                 QueueTtsResult::InvalidArgument,
             "PCM larger than one 20 ms frame was accepted") ||
      !Check(engine.QueueReplyFrame(frame.data(), frame.size() - 1U, 0U) ==
                 QueueTtsResult::InvalidArgument,
             "odd PCM byte count was accepted") ||
      !Check(engine.QueueReplyFrame(frame.data(), frame.size(), 1U) ==
                 QueueTtsResult::Discontinuous,
             "first PCM sequence did not have to start at zero") ||
      !Check(engine.QueueReplyFrame(frame.data(), frame.size(), 0U) == QueueTtsResult::Queued,
             "valid PCM was not queued") ||
      !Check(engine.QueueReplyFrame(frame.data(), frame.size(), 2U) ==
                 QueueTtsResult::Discontinuous,
             "sequence gap did not have a distinct queue result") ||
      !Check(engine.LastError().empty(), "queue flow control polluted last_error")) {
    return false;
  }
  engine.DropPlayback();
  return Check(WaitForPlaybackDone(engine, 500ms), "dropped playback did not finish");
}

/** @brief 阻塞播放消费者后填满 75 槽，额外帧必须明确背压，不能覆盖已有音频。 */
bool TestQueueCapacity() {
  AudioTasks engine;
  if (!OpenEngine(&engine) || !BeginPlayback(&engine)) {
    return false;
  }
  fake::BlockPlayback(fake::PlaybackBlock::Render);
  for (std::uint64_t sequence = 0U; sequence < 9U; ++sequence) {
    if (!QueueFrame(&engine, sequence)) {
      return false;
    }
  }
  if (!Check(fake::WaitForPlaybackBlocked(500ms), "capacity test did not block the consumer")) {
    return false;
  }
  // 一帧已由播放线程取走。剩余 75 个槽填满后，下一整帧必须返回背压。
  for (std::uint64_t sequence = 9U; sequence <= 75U; ++sequence) {
    if (!QueueFrame(&engine, sequence)) {
      return false;
    }
  }
  const auto extra = Pcm16(320U, 999);
  const bool rejected =
      Check(engine.QueueReplyFrame(extra.data(), extra.size(), 76U) == QueueTtsResult::Full,
            "full 1.5-second queue accepted another frame") &&
      Check(engine.LastError().empty(), "queue backpressure polluted last_error");
  engine.DropPlayback();
  return rejected &&
         Check(WaitForPlaybackDone(engine, 500ms), "full queue did not stop after drop") &&
         Check(!engine.HasPlaybackFailed(), "dropping a full queue became a playback failure");
}

/** @brief 用重复 Open 制造错误，关闭后成功重开必须清除上一生命周期的错误文本。 */
bool TestSuccessfulOpenClearsOldError() {
  AudioTasks engine;
  if (!OpenEngine(&engine) ||
      !Check(!engine.Start(), "duplicate Open unexpectedly succeeded") ||
      !Check(!engine.LastError().empty(), "duplicate Open did not publish its error")) {
    return false;
  }
  engine.Stop();
  return OpenEngine(&engine) &&
         Check(engine.LastError().empty(), "successful reopen retained an old error");
}

/** @brief 成功开始新播放应清除之前的错误，避免普通状态查询持续报告旧失败。 */
bool TestSuccessfulPlaybackClearsOldError() {
  AudioTasks engine;
  if (!OpenEngine(&engine) ||
      !Check(!engine.Start(), "duplicate Open unexpectedly succeeded") ||
      !Check(!engine.LastError().empty(), "duplicate Open did not publish its error") ||
      !BeginPlayback(&engine)) {
    return false;
  }
  const bool cleared =
      Check(engine.LastError().empty(), "successful playback retained an old error");
  engine.DropPlayback();
  return cleared &&
         Check(WaitForPlaybackDone(engine, 500ms), "dropped playback did not finish");
}

/** @brief 连续两轮核对 capture Arm → playback Prepare/Render/Drain，各步守住线程归属。 */
bool TestPlaybackOwnerOrder() {
  AudioTasks engine;
  if (!OpenEngine(&engine) || !PrimePlayback(&engine) || !engine.EndPlayback() ||
      !Check(WaitForPlaybackDone(engine, 500ms), "playback did not drain")) {
    return false;
  }
  // 再开一轮，证明播放准备只发生一次，且每轮都在本轮 capture 武装之后。
  return PrimePlayback(&engine) && engine.EndPlayback() &&
         Check(WaitForPlaybackDone(engine, 500ms), "second playback did not drain") &&
         Check(fake::PlaybackOwnerOrderIsValid(),
               "capture arm or playback prepare/render/drain crossed its owner");
}

/** @brief 在 render 或 drain 阻塞处执行 Drop/Close，要求及时唤醒且不误报播放失败。 */
bool TestInterruptBlockedPlayback(fake::PlaybackBlock stage, bool close) {
  AudioTasks engine;
  if (!OpenEngine(&engine) || !BeginPlayback(&engine)) {
    return false;
  }
  fake::BlockPlayback(stage);
  // EOS 放行一个短回复，同时覆盖 render 与随后 drain 的阻塞位置。
  if (!QueueFrame(&engine, 0U) || !engine.EndPlayback() ||
      !Check(fake::WaitForPlaybackBlocked(500ms), "playback did not reach blocking I/O")) {
    return false;
  }
  const auto started = std::chrono::steady_clock::now();
  if (close) {
    engine.Stop();
  } else {
    engine.DropPlayback();
  }
  return Check(WaitForPlaybackDone(engine, 200ms), "interrupt did not release playback") &&
         Check(std::chrono::steady_clock::now() - started < 250ms,
               "drop/close waited for blocked playback") &&
         Check(!engine.HasPlaybackFailed(), "user interruption became playback failure") &&
         Check(fake::PlaybackOwnerOrderIsValid(), "interruption changed playback owner");
}

/** @brief 起播准备失败要立即发布失败完成，不能等服务端 END 或继续向后端渲染。 */
bool TestPlaybackPrepareFailure() {
  AudioTasks engine;
  if (!OpenEngine(&engine) || !BeginPlayback(&engine)) {
    return false;
  }
  fake::FailPlaybackPreparation();
  for (std::uint64_t sequence = 0U; sequence < 9U; ++sequence) {
    if (!QueueFrame(&engine, sequence)) {
      return false;
    }
  }
  // 服务端尚未发 EOS；本地准备失败必须立即通知 application，不能等待下行结束。
  return Check(WaitForPlaybackDone(engine, 500ms) && engine.HasPlaybackFailed(),
               "playback preparation failure was not published") &&
         Check(fake::RenderCallsSnapshot().empty(), "failed preparation still rendered PCM");
}

/** @brief 开口时先返回 SpeechStart，再按原顺序吐出最近 25 帧（500 ms）的句首缓存。 */
bool TestVoiceAudioPreRollOrder() {
  VoiceAudio voice;
  if (!OpenVoice(&voice) || !BeginVoiceListen(&voice, ListenMode::Wake)) {
    return false;
  }

  std::vector<AudioEvent> events;
  for (std::int16_t id = 1; id <= 55; ++id) {
    CaptureFrame frame{};
    frame.pcm[0] = id;
    frame.timestamp_us = static_cast<std::uint64_t>(id) * 20000U;
    ProcessInjected(voice, frame, events);
    if (!Check(events.empty(), "pre-roll history emitted PCM before speech admission")) {
      return false;
    }
  }
  CaptureFrame admitted{};
  admitted.pcm[0] = 56;
  admitted.timestamp_us = 56U * 20000U;
  admitted.vad_now = admitted.vad_started = true;
  ProcessInjected(voice, admitted, events);
  if (!Check(events.size() == 26 && events.front().kind == AudioEventKind::SpeechStart,
             "speech admission did not lead with SpeechStart")) {
    return false;
  }

  std::uint64_t previous_sequence = 0U;
  for (std::int16_t expected = 32; expected <= 56; ++expected) {
    const AudioEvent& event = events[static_cast<std::size_t>(expected - 31)];
    if (!Check(event.kind == AudioEventKind::Pcm, "pre-roll PCM was missing") ||
        !Check(event.pcm[0] == expected,
               "pre-roll did not retain the newest contiguous 25 frames") ||
        !Check(event.timestamp_us == static_cast<std::uint64_t>(expected) * 20000U,
               "pre-roll timestamp changed") ||
        !Check(!event.end, "admission pre-roll acquired a false END")) {
      return false;
    }
    if (expected != 32 && !Check(event.sequence == previous_sequence + 1U,
                                 "pre-roll capture sequence was not contiguous")) {
      return false;
    }
    previous_sequence = event.sequence;
  }
  voice.CancelInput();
  voice.Close();
  return true;
}

/** @brief 短追问被拒绝后，新追问必须重新累计 400 ms，旧 PCM 和 END 不得混入。 */
bool TestVoiceAudioFollowUpDropsOldEnd() {
  VoiceAudio voice;
  if (!OpenVoice(&voice) || !BeginVoiceListen(&voice, ListenMode::FollowUp)) {
    return false;
  }

  std::vector<AudioEvent> events;
  for (std::int16_t id = 1; id <= 10; ++id) {
    CaptureFrame frame{};
    frame.pcm[0] = id;
    frame.timestamp_us = static_cast<std::uint64_t>(id) * 20000U;
    frame.near_voice = frame.vad_now = true;
    ProcessInjected(voice, frame, events);
    if (!Check(events.empty(), "short follow-up was admitted before 400 ms")) {
      return false;
    }
  }
  CaptureFrame old_end{};
  old_end.pcm[0] = 11;
  old_end.timestamp_us = 11U * 20000U;
  old_end.vad_ended = true;
  ProcessInjected(voice, old_end, events);
  if (!Check(events.empty(), "rejected short follow-up emitted an event")) {
    return false;
  }

  for (std::int16_t id = 100; id <= 119; ++id) {
    CaptureFrame frame{};
    frame.pcm[0] = id;
    frame.timestamp_us = static_cast<std::uint64_t>(id) * 20000U;
    frame.near_voice = frame.vad_now = true;
    ProcessInjected(voice, frame, events);
    if (id != 119 && !Check(events.empty(), "follow-up was admitted before 20 new frames")) {
      return false;
    }
    if (id == 119 &&
        !Check(events.size() == 21 && events.front().kind == AudioEventKind::SpeechStart,
               "new follow-up was not admitted")) {
      return false;
    }
  }
  for (std::int16_t expected = 100; expected <= 119; ++expected) {
    const AudioEvent& event = events[static_cast<std::size_t>(expected - 99)];
    if (!Check(event.kind == AudioEventKind::Pcm, "new follow-up PCM was missing") ||
        !Check(event.pcm[0] == expected, "old short utterance contaminated new follow-up") ||
        !Check(!event.end, "old VAD END truncated the new follow-up")) {
      return false;
    }
  }
  voice.CancelInput();
  voice.Close();
  return true;
}

/**
 * @brief 按生产插话流程注入有参考近讲、参考消失和持续近讲，直到产生 Barge。
 * 分类与电平由脚本给定，保留真实静音探测时序；本测试不验证麦克风声学判别效果。
 */
bool DriveConfirmedBarge(VoiceAudio& voice, std::vector<AudioEvent>& events) {
  std::int16_t id = 1;
  auto drive = [&](const unsigned count, const bool reference, const bool near_voice) {
    for (unsigned index = 0U; index < count; ++index, ++id) {
      CaptureFrame frame{};
      frame.pcm[0] = id;
      frame.timestamp_us = static_cast<std::uint64_t>(id) * 20000U;
      frame.reference_active = reference;
      frame.near_voice = near_voice;
      frame.vad_now = near_voice;
      frame.voice_dbfs = -10.0F;
      ProcessInjected(voice, frame, events);
      if (!events.empty()) {
        return false;
      }
    }
    return true;
  };
  if (!drive(6U, true, true) || !drive(3U, false, true) || !drive(3U, false, true)) {
    return false;
  }
  for (unsigned index = 0U; index < 3U; ++index, ++id) {
    CaptureFrame frame{};
    frame.pcm[0] = id;
    frame.timestamp_us = static_cast<std::uint64_t>(id) * 20000U;
    frame.near_voice = frame.vad_now = true;
    frame.voice_dbfs = -10.0F;
    ProcessInjected(voice, frame, events);
    if (index != 2U && !events.empty()) {
      return false;
    }
    if (index == 2U) {
      return events.size() == 16 && events.front().kind == AudioEventKind::Barge;
    }
  }
  return false;
}

/** @brief 旧播放仍阻塞时确认插话，新 generation 的回复须能等旧 drop 收尾后正常开始。 */
bool TestVoiceAudioBargeAndDropLifecycle() {
  VoiceAudio voice;
  if (!OpenVoice(&voice)) {
    return false;
  }
  const auto pcm = Pcm16(320U, 1000);
  if (!BeginVoicePlayback(&voice, 7U, pcm)) {
    return false;
  }
  fake::BlockPlayback(fake::PlaybackBlock::Render);
  for (std::uint32_t sequence = 1U; sequence < 9U; ++sequence) {
    if (!Check(voice.Play(7U, pcm.data(), pcm.size(), sequence, false, false),
               "VoiceAudio could not fill the initial playback buffer")) {
      return false;
    }
  }
  if (!Check(fake::WaitForPlaybackBlocked(500ms),
             "playback did not enter the blocked render")) {
    return false;
  }

  std::vector<AudioEvent> events;
  if (!Check(DriveConfirmedBarge(voice, events), "production barge detector did not confirm") ||
      !Check(events.front().generation == 7U,
             "Barge was not bound to the interrupted generation")) {
    return false;
  }
  for (std::size_t i = 1; i < events.size(); ++i) {
    if (!Check(events[i].kind == AudioEventKind::Pcm &&
                   events[i].pcm[0] == static_cast<std::int16_t>(i),
               "barge lost or reordered the retained near speech")) {
      return false;
    }
  }

  // 模拟 application 取消已确认的插话输入后再收新回复；旧线程尚在完成 drop，
  // 不能让这种有界收尾竞态变成下一轮播放失败。
  voice.CancelInput();
  fake::BlockPlayback(fake::PlaybackBlock::None);
  const auto started = std::chrono::steady_clock::now();
  if (!BeginVoicePlayback(&voice, 8U, pcm)) {
    return false;
  }
  const auto elapsed = std::chrono::steady_clock::now() - started;
  if (!Check(elapsed < 250ms, "drop-to-next-playback lifecycle was not bounded")) {
    return false;
  }
  voice.StopPlayback();
  voice.Close();
  return true;
}

}  // namespace

/** @brief CTest 每次只运行一个场景，退出码区分通过、失败与无效场景参数。 */
int main(const int argc, char** const argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: %s <scenario>\n", argv[0]);
    return 2;
  }
  const std::string scenario = argv[1];
  bool passed = false;
  if (scenario == "sub-grace-jitter") {
    passed = TestSubGraceJitter();
  } else if (scenario == "confirmed-gap") {
    passed = TestRebufferAfterConfirmedGap();
  } else if (scenario == "short-tail") {
    passed = TestEndPlaybackShortTail() && TestSingleFrameReply(1U, -32768) &&
             TestSingleFrameReply(320U, 32767);
  } else if (scenario == "bounded-close") {
    passed = TestBoundedClose();
  } else if (scenario == "bounded-capture") {
    passed = TestBoundedCaptureWait();
  } else if (scenario == "bounded-command") {
    passed = TestBoundedCaptureCommand();
  } else if (scenario == "reset-preserves-pcm") {
    passed = TestResetPreservesQueuedPcm();
  } else if (scenario == "queue-results") {
    passed = TestQueueResultsDoNotPolluteLastError() && TestQueueCapacity();
  } else if (scenario == "open-clears-error") {
    passed = TestSuccessfulOpenClearsOldError();
  } else if (scenario == "playback-clears-error") {
    passed = TestSuccessfulPlaybackClearsOldError();
  } else if (scenario == "playback-owner-order") {
    passed = TestPlaybackOwnerOrder();
  } else if (scenario == "drop-blocked-render") {
    passed = TestInterruptBlockedPlayback(fake::PlaybackBlock::Render, false);
  } else if (scenario == "drop-blocked-drain") {
    passed = TestInterruptBlockedPlayback(fake::PlaybackBlock::Drain, false);
  } else if (scenario == "close-blocked-render") {
    passed = TestInterruptBlockedPlayback(fake::PlaybackBlock::Render, true);
  } else if (scenario == "playback-prepare-failure") {
    passed = TestPlaybackPrepareFailure();
  } else if (scenario == "voice-preroll") {
    passed = TestVoiceAudioPreRollOrder();
  } else if (scenario == "voice-follow-up-boundary") {
    passed = TestVoiceAudioFollowUpDropsOldEnd();
  } else if (scenario == "voice-barge-lifecycle") {
    passed = TestVoiceAudioBargeAndDropLifecycle();
  } else if (scenario == "pipeline-format") {
    passed = boompi::test::TestPipelineFormat();
  } else if (scenario == "pipeline-detection") {
    passed = boompi::test::TestPipelineDetection();
  } else {
    std::fprintf(stderr, "unknown scenario: %s\n", scenario.c_str());
    return 2;
  }
  if (passed) {
    std::printf("audio-engine harness: %s passed\n", scenario.c_str());
  }
  return passed ? 0 : 1;
}
