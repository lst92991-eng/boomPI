#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <limits>

#include "boompi/audio/audio_frame.h"
#include "boompi/audio/wake_word_engine.h"
#include "boompi/platform/rv1106/snowboy_wake_word_engine.h"

namespace {

constexpr std::uint32_t kDefaultFrameCount = 5U;
constexpr std::uint32_t kMaximumFrameCount = 500U;

bool ParseFrameCount(const char* const text,
                     std::uint32_t* const frame_count) noexcept {
  if (text == nullptr || frame_count == nullptr || text[0] == '\0') {
    return false;
  }
  char* end = nullptr;
  const unsigned long parsed = std::strtoul(text, &end, 10);
  if (end == text || *end != '\0' || parsed == 0UL ||
      parsed > kMaximumFrameCount ||
      parsed > std::numeric_limits<std::uint32_t>::max()) {
    return false;
  }
  *frame_count = static_cast<std::uint32_t>(parsed);
  return true;
}

boompi::audio::Mono16kFrame SilentFrame(
    const std::uint32_t sequence) noexcept {
  boompi::audio::Mono16kFrame frame{};
  frame.format = {16000U, 20U, 1U,
                  boompi::audio::SampleFormat::kPcmS16Le};
  frame.metadata.monotonic_timestamp_us =
      1000000U + static_cast<std::uint64_t>(sequence) * 20000U;
  frame.metadata.sequence = sequence;
  frame.metadata.stream_id = 1U;
  frame.metadata.turn_id = 0U;
  frame.metadata.epoch = 1U;
  frame.samples_per_channel = 320U;
  frame.samples.fill(0);
  return frame;
}

}  // namespace

int main(const int argc, char* argv[]) {
  if (argc < 3 || argc > 5) {
    std::cerr
        << "usage=snowboy-offline-probe RESOURCE MODEL [FRAMES] "
           "[PCM_S16LE]\n";
    return 64;
  }

  std::uint32_t frame_count = kDefaultFrameCount;
  if (argc >= 4 && !ParseFrameCount(argv[3], &frame_count)) {
    std::cerr << "frame_count=invalid\n";
    return 64;
  }
  std::ifstream pcm_input;
  if (argc == 5) {
    pcm_input.open(argv[4], std::ios::binary);
    if (!pcm_input.is_open()) {
      std::cerr << "pcm_input=open_failed\n";
      return 66;
    }
  }

  boompi::platform::rv1106::SnowboyWakeWordConfig config{};
  config.resource_path = argv[1];
  config.model_path = argv[2];
  config.sensitivity = "0.5";
  config.audio_gain = 1.0F;
  config.apply_frontend = false;

  boompi::platform::rv1106::SnowboyWakeWordCreateResult create{};
  const auto create_started = std::chrono::steady_clock::now();
  auto engine =
      boompi::platform::rv1106::SnowboyWakeWordEngine::Create(
          config, &create);
  const auto create_elapsed_us =
      std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::steady_clock::now() - create_started)
          .count();
  if (!engine) {
    std::cerr << "create=failed\n"
              << "create_error="
              << static_cast<unsigned int>(create.error) << '\n'
              << "sample_rate_hz=" << create.sample_rate_hz << '\n'
              << "channels=" << create.channels << '\n'
              << "bits_per_sample=" << create.bits_per_sample << '\n'
              << "keyword_count=" << create.keyword_count << '\n';
    return 2;
  }

  std::uint32_t silence_count = 0U;
  std::uint32_t no_event_count = 0U;
  std::uint32_t detection_count = 0U;
  std::int64_t process_total_us = 0;
  std::int64_t process_max_us = 0;
  for (std::uint32_t sequence = 0U; sequence < frame_count; ++sequence) {
    auto frame = SilentFrame(sequence);
    if (pcm_input.is_open()) {
      constexpr std::streamsize kFrameBytes =
          static_cast<std::streamsize>(
              boompi::audio::kMono16kFrameInterleavedSampleCapacity *
              sizeof(std::int16_t));
      pcm_input.read(reinterpret_cast<char*>(frame.samples.data()),
                     kFrameBytes);
      if (pcm_input.gcount() != kFrameBytes) {
        std::cerr << "pcm_input=short_frame\n"
                  << "sequence=" << sequence << '\n';
        return 66;
      }
    }
    const auto process_started = std::chrono::steady_clock::now();
    const auto result = engine->Process(frame);
    const auto process_elapsed_us =
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - process_started)
            .count();
    process_total_us += process_elapsed_us;
    if (process_elapsed_us > process_max_us) {
      process_max_us = process_elapsed_us;
    }
    if (!result.valid() || !result.produced()) {
      std::cerr << "process=failed\n"
                << "sequence=" << sequence << '\n'
                << "decision="
                << static_cast<unsigned int>(result.decision) << '\n'
                << "error=" << static_cast<unsigned int>(result.error)
                << '\n';
      return 3;
    }
    if (result.decision == boompi::audio::WakeWordDecision::kSilence) {
      ++silence_count;
    } else if (result.decision ==
               boompi::audio::WakeWordDecision::kNoEvent) {
      ++no_event_count;
    } else if (result.detected()) {
      ++detection_count;
    }
  }

  const auto reset =
      engine->Reset(
          boompi::audio::WakeWordResetReason::kVadSegmentEnded);
  if (!reset.valid() || !reset.reset) {
    std::cerr << "reset=failed\n"
              << "reset_error="
              << static_cast<unsigned int>(reset.error) << '\n';
    return 4;
  }

  std::cout << "create=ok\n"
            << "sample_rate_hz=" << create.sample_rate_hz << '\n'
            << "channels=" << create.channels << '\n'
            << "bits_per_sample=" << create.bits_per_sample << '\n'
            << "keyword_count=" << create.keyword_count << '\n'
            << "frames=" << frame_count << '\n'
            << "create_elapsed_us=" << create_elapsed_us << '\n'
            << "process_total_us=" << process_total_us << '\n'
            << "process_max_us=" << process_max_us << '\n'
            << "silence=" << silence_count << '\n'
            << "no_event=" << no_event_count << '\n'
            << "detections=" << detection_count << '\n'
            << "reset=ok\n";
  return 0;
}
