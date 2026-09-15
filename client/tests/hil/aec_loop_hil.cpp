// 真板固定音源探针：复用真实采集、speech准入和播放模块，不连接服务端。
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string_view>
#include <vector>

#include "boompi/audio/playback.h"
#include "boompi/audio/speech.h"
#include "boompi/audio/voice_input.h"
#include "vad.h"
#include "wake.h"

namespace {
namespace capture = boompi::voice_input;
namespace playback = boompi::playback;
namespace speech = boompi::speech;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
constexpr std::uint8_t kVolume = 60;
constexpr std::size_t kFrameBytes = boompi::audio::kTtsFrameSamples * 2;
constexpr unsigned kPlaybackRepeats = 6;
struct CloseAudio {
  ~CloseAudio() {
    playback::close();
    capture::close();
    boompi::wake::close();
    boompi::vad::close();
  }
};

bool ReadFixture(const char* path, std::vector<std::uint8_t>& fixture) {
  std::ifstream input(path, std::ios::binary | std::ios::ate);
  if (!input) {
    return false;
  }
  const auto length = input.tellg();
  if (length <= 0 || static_cast<std::uint64_t>(length) > 70 * kFrameBytes ||
      static_cast<std::uint64_t>(length) % kFrameBytes != 0) {
    return false;
  }
  fixture.resize(static_cast<std::size_t>(length));
  input.seekg(0);
  return input.read(reinterpret_cast<char*>(fixture.data()), length).good();
}
bool ProcessAudio(boompi::audio::CaptureFrame& frame, speech::Result& result, bool speaking) {
  result = {};
  const auto read = capture::read(&frame, 20ms);
  if (read == capture::ReadResult::Failed) {
    std::cerr << "boompi-aec-loop-hil: " << capture::error() << '\n';
    return false;
  }
  if (read == capture::ReadResult::Frame) {
    if (!boompi::wake::detect(frame.pcm, &frame.wake)) {
      return false;
    }
    const int voiced = boompi::vad::process(frame.pcm);
    if (voiced < 0) {
      return false;
    }
    frame.vad_now = voiced == 1;
    result = speech::update(frame, speaking, frame.output);
    playback::set_scale(result.playback_scale);
    if (result.decision == speech::Decision::Fault) {
      std::cerr << "boompi-aec-loop-hil: capture discontinuity\n";
      return false;
    }
  }
  if (playback::status().state == playback::State::Failed) {
    std::cerr << "boompi-aec-loop-hil: " << playback::error() << '\n';
    return false;
  }
  return true;
}
}  // namespace

int main(int argc, char** argv) {
  if (argc != 2 || std::string_view(argv[1]) == "--help") {
    std::cout << "usage: boompi-aec-loop-hil <16k-mono-s16le.raw>\n";
    return argc == 2 ? EXIT_SUCCESS : EXIT_FAILURE;
  }
  std::vector<std::uint8_t> fixture;
  if (!ReadFixture(argv[1], fixture)) {
    std::cerr << "fixture must be nonempty, 20ms aligned and <=1.4s\n";
    return EXIT_FAILURE;
  }
  CloseAudio cleanup;
  if (!boompi::wake::open() || !boompi::vad::open() || !capture::open() ||
      !playback::open(kVolume) || !capture::start()) {
    std::cerr << "audio initialization failed: " << capture::error() << ' ' << playback::error()
              << '\n';
    return EXIT_FAILURE;
  }
  speech::reset();
  boompi::audio::CaptureFrame frame{};
  speech::Result result{};
  unsigned settle_wakes = 0;
  const auto settled_at = Clock::now() + 5s;
  while (Clock::now() < settled_at) {
    if (!ProcessAudio(frame, result, false)) {
      return EXIT_FAILURE;
    }
    settle_wakes += result.decision == speech::Decision::Wake;
  }
  const std::size_t fixture_frames = fixture.size() / kFrameBytes;
  const std::size_t total_frames = fixture_frames * kPlaybackRepeats;
  const auto send = [&](std::size_t index) {
    const auto* pcm = fixture.data() + index % fixture_frames * kFrameBytes;
    return playback::write(1, pcm, kFrameBytes) == playback::WriteResult::Queued &&
           (index + 1 != total_frames || playback::finish(1));
  };
  speech::reset();
  speech::reply_started();
  if (!playback::begin(1)) {
    return EXIT_FAILURE;
  }
  std::size_t next = 0;
  for (; next < std::min<std::size_t>(total_frames, 9); ++next) {
    if (!send(next)) {
      return EXIT_FAILURE;
    }
  }
  bool would_barge = false;
  bool playback_done = false;
  auto next_send = Clock::now() + 20ms;
  const auto playback_limit = Clock::now() + 5s + std::chrono::milliseconds(total_frames * 20);
  while (Clock::now() < playback_limit && !playback_done) {
    if (!ProcessAudio(frame, result, playback::status().state == playback::State::Playing)) {
      return EXIT_FAILURE;
    }
    if (result.decision == speech::Decision::Barge) {
      would_barge = true;
      playback::cancel();
      break;
    }
    const auto state = playback::status();
    playback_done = state.generation == 1 && state.state == playback::State::Drained;
    if (!playback_done && next < total_frames && Clock::now() >= next_send) {
      if (!send(next++)) {
        return EXIT_FAILURE;
      }
      next_send += 20ms;
    }
  }
  bool would_follow_up = false;
  if (!would_barge && playback_done) {
    if (!speech::listen(speech::ListenMode::FollowUp)) {
      return EXIT_FAILURE;
    }
    const auto post_limit = Clock::now() + 1s;
    while (Clock::now() < post_limit) {
      if (!ProcessAudio(frame, result, false)) {
        return EXIT_FAILURE;
      }
      if (result.decision == speech::Decision::Start) {
        would_follow_up = true;
        break;
      }
    }
  }
  std::cout << "AEC_LOOP_RESULT {\"profile_fields\":6,\"volume_percent\":"
            << static_cast<unsigned>(kVolume) << ",\"fixture_frames\":" << fixture_frames
            << ",\"playback_frames\":" << total_frames << ",\"settle_wakes\":" << settle_wakes
            << ",\"playback_done\":" << (playback_done ? "true" : "false")
            << ",\"would_barge\":" << (would_barge ? "true" : "false")
            << ",\"would_follow_up\":" << (would_follow_up ? "true" : "false") << "}\n";
  return playback_done && !would_barge && !would_follow_up ? EXIT_SUCCESS : EXIT_FAILURE;
}
