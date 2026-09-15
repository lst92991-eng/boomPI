// Host只核对样本和资源边界；不模拟声学体验或固定旧调参策略。
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "audio_hardware.h"
#include "audio_vendor.h"
#include "boompi/audio/playback.h"
#include "boompi/audio/speech.h"
#include "boompi/audio/voice_input.h"

namespace boompi::test {
bool TestModuleFormat();
bool TestModuleDetection();
}  // namespace boompi::test
namespace {
using namespace boompi;
using namespace std::chrono_literals;
namespace hw = test::audio_hardware;
namespace vendor = test::audio_vendor;
void require(bool pass, const char* reason) {
  if (!pass) {
    throw std::runtime_error(reason);
  }
}
void close_audio() {
  playback::close();
  voice_input::close();
}
void open_audio() {
  close_audio();
  hw::reset();
  vendor::reset();
  require(voice_input::open(), "input open");
  require(hw::capture_reads() == 0, "read before both PCM devices configured");
  require(playback::open(100) && voice_input::start(), "audio start");
  require(hw::wait_for_capture_reads(1, 500ms), "capture did not start");
}
void wait_playback(playback::State expected) {
  const auto end = std::chrono::steady_clock::now() + 500ms;
  while (playback::status() == playback::State::Playing &&
         std::chrono::steady_clock::now() < end) {
    std::this_thread::sleep_for(1ms);
  }
  require(playback::status() == expected, "playback did not complete expected transition");
}
std::vector<std::uint8_t> pcm(std::size_t count, std::int16_t sample) {
  std::vector<std::uint8_t> bytes(count * 2);
  for (std::size_t i = 0; i < count; ++i) {
    bytes[2 * i] = static_cast<std::uint8_t>(sample);
    bytes[2 * i + 1] = static_cast<std::uint8_t>(static_cast<std::uint16_t>(sample) >> 8);
  }
  return bytes;
}
void queue(const std::vector<std::uint8_t>& bytes) {
  require(playback::write(bytes.data(), bytes.size()) == playback::WriteResult::Queued,
          "PCM rejected");
}
void playback_boundaries() {
  for (std::size_t count : {1, 73, 320}) {
    open_audio();
    queue(pcm(count, 16000));
    hw::block_playback(hw::PlaybackBlock::Drain);
    playback::finish();
    require(hw::wait_for_playback_blocked(500ms), "short reply never reached drain");
    require(playback::status() == playback::State::Playing, "DONE ended physical tail early");
    hw::block_playback(hw::PlaybackBlock::None);
    wait_playback(playback::State::Drained);
    const auto output = hw::written_samples();
    require(output.size() == count * 6, "short reply duration changed");
    require(*std::max_element(output.begin(), output.end()) > 8000, "short reply muted");
  }
  for (auto stage : {hw::PlaybackBlock::Write, hw::PlaybackBlock::Drain}) {
    open_audio();
    hw::block_playback(stage);
    queue(pcm(320, 16000));
    playback::finish();
    require(hw::wait_for_playback_blocked(500ms), "I/O not reached");
    playback::cancel();
    wait_playback(playback::State::Idle);
    const auto old_size = hw::written_samples().size();
    hw::block_playback(hw::PlaybackBlock::None);
    queue(pcm(320, 0));
    playback::finish();
    wait_playback(playback::State::Drained);
    const auto output = hw::written_samples();
    require(std::all_of(output.begin() + old_size, output.end(),
                        [](auto x) {
                          return x == 0;
                        }),
            "canceled samples or filter tail leaked into next reply");
  }
  open_audio();
  hw::block_playback(hw::PlaybackBlock::Write);
  queue(pcm(320, -32768));
  require(hw::wait_for_playback_blocked(500ms), "blocked write not reached");
  const auto full = pcm(24000, 0);
  queue(full);
  const auto extra = pcm(1, 0);
  require(playback::write(extra.data(), extra.size()) == playback::WriteResult::Full,
          "full queue silently overwrote speech");
  const auto started = std::chrono::steady_clock::now();
  close_audio();
  require(std::chrono::steady_clock::now() - started < 500ms,
          "close did not interrupt blocking I/O");
}
void speech_samples() {
  speech::reset();
  std::vector<int> delivered;
  bool started = false, ended = false;
  for (int id = 0; id < 65; ++id) {
    audio::CaptureFrame frame;
    frame.pcm.fill(static_cast<std::int16_t>(id));
    frame.vad_now = id >= 20 && id < 30;
    const auto result = speech::update(frame);
    if (result.start) {
      require(!started && id == 25, "more than one onset or wrong current frame");
      started = true;
    }
    for (std::size_t i = 0; i < result.count; ++i) {
      delivered.push_back((*result.frames[i])[0]);
    }
    ended = result.end;
  }
  require(started && ended && delivered.size() == 64, "pre-roll or tail missing");
  for (int id = 1; id <= 64; ++id) {
    require(delivered[id - 1] == id, "pre-roll/current/tail duplicated or skipped");
  }
}
void capture_boundaries() {
  open_audio();
  audio::RawCaptureFrame raw{};
  audio::CaptureFrame frame;
  hw::push_capture(raw, true);
  require(voice_input::read(frame) == voice_input::ReadResult::Frame && frame.discontinuity,
          "capture gap hidden");
  for (int i = 0; i < 6; ++i) {
    hw::push_capture(raw);
  }
  require(hw::wait_for_capture_reads(8, 500ms), "capture stopped while consumer stalled");
  require(voice_input::read(frame) == voice_input::ReadResult::Frame && frame.discontinuity,
          "queue overflow silently stitched PCM");
  for (int failure = 0; failure < 4; ++failure) {
    open_audio();
    if (failure == 0) {
      vendor::vad_result = -1;
    } else if (failure == 1) {
      vendor::snowboy_process_ok = false;
    } else if (failure == 2) {
      vendor::dsp_failure_call = 1;
    } else {
      vendor::wake_result = -1;
    }
    hw::push_capture(raw);
    require(voice_input::read(frame) == voice_input::ReadResult::Failed &&
                !voice_input::error().empty(),
            "vendor failure became silence");
    close_audio();
  }
  hw::fail_capture_open(true);
  require(!voice_input::open(), "capture open failure ignored");
  close_audio();
}
}  // namespace
int main(int argc, char** argv) {
  try {
    const std::string scenario = argc == 2 ? argv[1] : "";
    if (scenario == "modules-format") {
      require(boompi::test::TestModuleFormat(), "format boundary failed");
    } else if (scenario == "modules-detection") {
      require(boompi::test::TestModuleDetection(), "vendor result contract failed");
    } else if (scenario == "voice-preroll") {
      speech_samples();
    } else if (scenario == "playback") {
      playback_boundaries();
    } else if (scenario == "capture") {
      capture_boundaries();
    } else {
      throw std::runtime_error("unknown audio check");
    }
    close_audio();
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "audio: %s\n", error.what());
    close_audio();
    return 1;
  }
}
