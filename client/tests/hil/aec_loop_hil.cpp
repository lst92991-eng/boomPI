/**
 * @file aec_loop_hil.cpp
 * @brief 真板固定音源探针；直接使用生产 VoiceAudio，不复制 VAD/barge 状态机。
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
using boompi::audio::VoiceAudio;
using boompi::audio::VoiceFrameContract;

constexpr std::uint8_t kVolume = 60U;
constexpr std::size_t kFrameBytes = kTtsFrameSamples * sizeof(std::int16_t);
constexpr std::size_t kMaximumFixtureFrames = VoiceFrameContract::FramesForMs(1400U);
constexpr std::size_t kInitialQueueFrames = VoiceFrameContract::FramesForMs(180U);
using Clock = std::chrono::steady_clock;
constexpr unsigned kPlaybackRepeats = 6U;

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

const std::uint8_t* Frame(const std::vector<std::uint8_t>& fixture, std::size_t index) {
  return fixture.data() + index % (fixture.size() / kFrameBytes) * kFrameBytes;
}

bool Poll(VoiceAudio* voice, AudioEvent* event, bool* have_event) {
  *have_event = voice->Poll(event, std::chrono::milliseconds(20));
  if ((*have_event && event->kind == AudioEventKind::Fault) || !voice->healthy()) {
    std::cerr << "boompi-aec-loop-hil: " << voice->last_error() << '\n';
    return false;
  }
  return true;
}
}  // namespace

int main(int argc, char* argv[]) {
  if (argc == 2 && std::string_view(argv[1]) == "--help") {
    std::cout << "usage: boompi-aec-loop-hil <24k-mono-s16le.raw>\n";
    return EXIT_SUCCESS;
  }
  if (argc != 2) {
    std::cerr << "usage: boompi-aec-loop-hil <24k-mono-s16le.raw>\n";
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
    std::cerr << "boompi-aec-loop-hil: " << voice.last_error() << '\n';
    return EXIT_FAILURE;
  }
  AudioEvent event{};
  bool have_event = false;
  std::uint64_t settle_wakes = 0U;
  const auto settled_at = Clock::now() + std::chrono::seconds(5);
  while (Clock::now() < settled_at) {
    if (!Poll(&voice, &event, &have_event)) {
      return EXIT_FAILURE;
    }
    if (have_event && event.kind == AudioEventKind::Wake) {
      ++settle_wakes;
    }
  }

  const std::size_t fixture_frames = fixture.size() / kFrameBytes;
  const std::size_t total_frames = fixture_frames * kPlaybackRepeats;
  std::size_t next = 0U;
  const std::size_t initial = std::min(total_frames, kInitialQueueFrames);
  for (; next < initial; ++next) {
    if (!voice.Play(1U, Frame(fixture, next), kFrameBytes, static_cast<std::uint32_t>(next),
                    next == 0U, next + 1U == total_frames)) {
      std::cerr << "boompi-aec-loop-hil: " << voice.last_error() << '\n';
      return EXIT_FAILURE;
    }
  }

  bool would_barge = false, playback_done = false;
  auto next_send = Clock::now() + std::chrono::milliseconds(20);
  const auto playback_limit =
      Clock::now() + std::chrono::seconds(5) +
      std::chrono::milliseconds(total_frames * VoiceFrameContract::frame_ms);
  // Poll是语义事件，不等于一帧。音源发送和总超时都使用真实墙钟。
  while (Clock::now() < playback_limit && !playback_done) {
    if (!Poll(&voice, &event, &have_event)) {
      return EXIT_FAILURE;
    }
    if (have_event && event.kind == AudioEventKind::Barge) {
      would_barge = true;
      break;
    }
    if (have_event && event.kind == AudioEventKind::PlaybackDone) {
      playback_done = event.generation == 1U;
      continue;
    }
    if (next < total_frames && Clock::now() >= next_send) {
      if (!voice.Play(1U, Frame(fixture, next), kFrameBytes, static_cast<std::uint32_t>(next),
                      false, next + 1U == total_frames)) {
        std::cerr << "boompi-aec-loop-hil: " << voice.last_error() << '\n';
        return EXIT_FAILURE;
      }
      ++next;
      next_send += std::chrono::milliseconds(20);
    }
  }

  bool would_follow_up = false;
  if (!would_barge && playback_done) {
    if (!voice.Listen(true)) {
      std::cerr << "boompi-aec-loop-hil: " << voice.last_error() << '\n';
      return EXIT_FAILURE;
    }
    const auto post_limit = Clock::now() + std::chrono::seconds(1);
    while (Clock::now() < post_limit) {
      if (!Poll(&voice, &event, &have_event)) {
        return EXIT_FAILURE;
      }
      if (have_event && event.kind == AudioEventKind::SpeechStart) {
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
