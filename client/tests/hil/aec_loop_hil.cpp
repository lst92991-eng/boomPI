/**
 * @file aec_loop_hil.cpp
 * @brief 真板固定音源探针；直接使用生产 VoiceAudio，不复制 VAD/barge 状态机。
 *
 * 执行顺序：校验固定音源 → 静置采集五秒 → 重复播放六遍 → 观察尾播后一秒。
 * 仅记录误唤醒/误插话/误追问等事件；不连接服务端，也不证明新 generation 已提交。
 */
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string_view>
#include <vector>

#include "boompi/audio/voice_audio.h"

namespace {
using boompi::audio::AudioEvent;
using boompi::audio::AudioEventKind;
using boompi::audio::kTtsFrameSamples;
using boompi::audio::ListenMode;
using boompi::audio::VoiceAudio;
using boompi::audio::VoiceFrameContract;

constexpr std::uint8_t kVolume = 60U;
constexpr std::size_t kFrameBytes = kTtsFrameSamples * sizeof(std::int16_t);
constexpr std::size_t kMaximumFixtureFrames = VoiceFrameContract::FramesForMs(1400U);
constexpr std::size_t kInitialQueueFrames = VoiceFrameContract::FramesForMs(180U);
using Clock = std::chrono::steady_clock;
constexpr unsigned kPlaybackRepeats = 6U;

/** @brief 加载不超过 1.4 秒的 16 kHz/S16_LE/mono 原始 PCM，要求严格按 20 ms 对齐。 */
bool ReadFixture(const char* path, std::vector<std::uint8_t>* fixture) {
  std::ifstream input(path, std::ios::binary | std::ios::ate);
  if (!input) {
    return false;
  }
  const std::streamoff length = input.tellg();
  if (length <= 0 || static_cast<std::uint64_t>(length) > kMaximumFixtureFrames * kFrameBytes ||
      static_cast<std::uint64_t>(length) % kFrameBytes != 0U) {
    return false;
  }
  fixture->resize(static_cast<std::size_t>(length));
  input.seekg(0);
  return input.read(reinterpret_cast<char*>(fixture->data()), length).good();
}

/** @brief 循环寻址第 index 个播放帧；fixture 已通过非空和帧对齐校验。 */
const std::uint8_t* Frame(const std::vector<std::uint8_t>& fixture, std::size_t index) {
  return fixture.data() + index % (fixture.size() / kFrameBytes) * kFrameBytes;
}

/** @brief 推进生产语音逻辑；区分暂无事件与音频 Fault，任一故障立即结束本次探针。 */
bool ProcessAudio(VoiceAudio* voice, std::vector<AudioEvent>& events) {
  voice->ProcessEvents(events, std::chrono::milliseconds(20));
  const bool fault = std::any_of(events.begin(), events.end(), [](const AudioEvent& event) {
    return event.kind == AudioEventKind::Fault;
  });
  if (fault || !voice->IsHealthy()) {
    std::cerr << "boompi-aec-loop-hil: " << voice->LastError() << '\n';
    return false;
  }
  return true;
}

}  // namespace

/** @brief 手动运行的真板诊断入口，输出机器可读结果和退出码，不属于自动 Host 测试。 */
int main(int argc, char* argv[]) {
  if (argc == 2 && std::string_view(argv[1]) == "--help") {
    std::cout << "usage: boompi-aec-loop-hil <16k-mono-s16le.raw>\n";
    return EXIT_SUCCESS;
  }
  if (argc != 2) {
    std::cerr << "usage: boompi-aec-loop-hil <16k-mono-s16le.raw>\n";
    return EXIT_FAILURE;
  }
  std::vector<std::uint8_t> fixture;
  if (!ReadFixture(argv[1], &fixture)) {
    std::cerr << "boompi-aec-loop-hil: fixture must be 20 ms aligned, "
                 "nonempty, and <= 1.4 s\n";
    return EXIT_FAILURE;
  }

  VoiceAudio voice;
  if (!voice.Open(kVolume)) {
    std::cerr << "boompi-aec-loop-hil: " << voice.LastError() << '\n';
    return EXIT_FAILURE;
  }
  std::vector<AudioEvent> events;
  std::uint64_t settle_wakes = 0U;
  const auto settled_at = Clock::now() + std::chrono::seconds(5);
  while (Clock::now() < settled_at) {
    if (!ProcessAudio(&voice, events)) {
      return EXIT_FAILURE;
    }
    settle_wakes += std::count_if(events.begin(), events.end(), [](const AudioEvent& event) {
      return event.kind == AudioEventKind::Wake;
    });
  }

  // 先填满最多 180 ms 起播缓冲，之后按单调时钟每 20 ms 补包，模拟持续流式回复。
  const std::size_t fixture_frames = fixture.size() / kFrameBytes;
  const std::size_t total_frames = fixture_frames * kPlaybackRepeats;
  std::size_t next = 0U;
  const std::size_t initial = std::min(total_frames, kInitialQueueFrames);
  for (; next < initial; ++next) {
    if (!voice.Play(1U, Frame(fixture, next), kFrameBytes, static_cast<std::uint32_t>(next),
                    next == 0U, next + 1U == total_frames)) {
      std::cerr << "boompi-aec-loop-hil: " << voice.LastError() << '\n';
      return EXIT_FAILURE;
    }
  }

  bool would_barge = false, playback_done = false;
  auto next_send = Clock::now() + std::chrono::milliseconds(20);
  const auto playback_limit =
      Clock::now() + std::chrono::seconds(5) +
      std::chrono::milliseconds(total_frames * VoiceFrameContract::frame_ms);
  // 每次处理不一定返回一个PCM帧；发送节奏和总超时均由单调时钟决定。
  while (Clock::now() < playback_limit && !playback_done) {
    if (!ProcessAudio(&voice, events)) {
      return EXIT_FAILURE;
    }
    if (std::any_of(events.begin(), events.end(), [](const AudioEvent& event) {
          return event.kind == AudioEventKind::Barge;
        })) {
      would_barge = true;
      break;
    }
    playback_done = std::any_of(events.begin(), events.end(), [](const AudioEvent& event) {
      return event.kind == AudioEventKind::PlaybackDone && event.generation == 1U;
    });
    if (playback_done) {
      continue;
    }
    if (next < total_frames && Clock::now() >= next_send) {
      if (!voice.Play(1U, Frame(fixture, next), kFrameBytes, static_cast<std::uint32_t>(next),
                      false, next + 1U == total_frames)) {
        std::cerr << "boompi-aec-loop-hil: " << voice.LastError() << '\n';
        return EXIT_FAILURE;
      }
      ++next;
      next_send += std::chrono::milliseconds(20);
    }
  }

  // 只有完整自然播完才验证尾音是否误触发追问；发生插话的实验不能复用这个阶段。
  bool would_follow_up = false;
  if (!would_barge && playback_done) {
    if (!voice.Listen(ListenMode::FollowUp)) {
      std::cerr << "boompi-aec-loop-hil: " << voice.LastError() << '\n';
      return EXIT_FAILURE;
    }
    const auto post_limit = Clock::now() + std::chrono::seconds(1);
    while (Clock::now() < post_limit) {
      if (!ProcessAudio(&voice, events)) {
        return EXIT_FAILURE;
      }
      if (std::any_of(events.begin(), events.end(), [](const AudioEvent& event) {
            return event.kind == AudioEventKind::SpeechStart;
          })) {
        would_follow_up = true;
        break;
      }
    }
    voice.CancelInput();
  }
  voice.Close();

  std::cout << "AEC_LOOP_RESULT {\"profile_fields\":6,\"volume_percent\":"
            << static_cast<unsigned>(kVolume) << ",\"fixture_frames\":" << fixture_frames
            << ",\"playback_frames\":" << total_frames << ",\"settle_wakes\":" << settle_wakes
            << ",\"playback_done\":" << (playback_done ? "true" : "false")
            << ",\"would_barge\":" << (would_barge ? "true" : "false")
            << ",\"would_follow_up\":" << (would_follow_up ? "true" : "false") << "}\n";
  return playback_done && !would_barge && !would_follow_up ? EXIT_SUCCESS : EXIT_FAILURE;
}
