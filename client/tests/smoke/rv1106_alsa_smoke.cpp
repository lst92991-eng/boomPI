#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

#include "boompi/audio/playback_committer.h"
#include "boompi/audio/playback_renderer_24_to_48.h"
#include "boompi/platform/audio_pcm_transfer.h"
#include "boompi/platform/rv1106/alsa_pcm_stream.h"

namespace {

using boompi::audio::AcceptedRenderQueue;
using boompi::audio::PlaybackCommitCode;
using boompi::audio::PlaybackCommitter;
using boompi::audio::PlaybackCommitterConfig;
using boompi::audio::PlaybackGainLimiterConfig;
using boompi::audio::PlaybackGeneration;
using boompi::audio::PlaybackPcmFrame48k;
using boompi::audio::PlaybackRenderer24To48;
using boompi::audio::TtsPcmFrame24k;
using boompi::platform::AudioIoStopFlag;
using boompi::platform::AudioPeriodTransferConfig;
using boompi::platform::CaptureEpochFence;
using boompi::platform::CapturePeriodReader48k;
using boompi::platform::PcmBackendCode;
using boompi::platform::PcmPlaybackSink48k;
using boompi::platform::SteadyAudioIoClock;
using boompi::platform::rv1106::AlsaPcmDirection;
using boompi::platform::rv1106::AlsaPcmStream;
using boompi::platform::rv1106::AlsaPcmStreamConfig;

struct Options final {
  std::string device_name;
  std::uint32_t periods{0U};
  std::uint8_t capture_channels{0U};
  std::uint8_t playback_channels{0U};
  std::uint32_t period_sample_frames{0U};
  std::uint32_t capture_buffer_sample_frames{0U};
  std::uint32_t playback_buffer_sample_frames{0U};
};

bool ParseUnsigned(const char* const text, std::uint32_t* const output) {
  if (text == nullptr || output == nullptr || text[0] == '\0') {
    return false;
  }
  errno = 0;
  char* end = nullptr;
  const unsigned long parsed = std::strtoul(text, &end, 10);
  if (errno != 0 || end == text || *end != '\0' ||
      parsed > UINT32_MAX) {
    return false;
  }
  *output = static_cast<std::uint32_t>(parsed);
  return true;
}

bool ParseOptions(const int argc, char** const argv, Options* const output) {
  if (output == nullptr) {
    return false;
  }
  for (int argument = 1; argument + 1 < argc; argument += 2) {
    const std::string name(argv[argument]);
    const char* const value = argv[argument + 1];
    std::uint32_t parsed = 0U;
    if (name == "--device") {
      output->device_name = value;
    } else if (name == "--periods" && ParseUnsigned(value, &parsed)) {
      output->periods = parsed;
    } else if (name == "--capture-channels" &&
               ParseUnsigned(value, &parsed) && parsed <= UINT8_MAX) {
      output->capture_channels = static_cast<std::uint8_t>(parsed);
    } else if (name == "--playback-channels" &&
               ParseUnsigned(value, &parsed) && parsed <= UINT8_MAX) {
      output->playback_channels = static_cast<std::uint8_t>(parsed);
    } else if (name == "--period-frames" &&
               ParseUnsigned(value, &parsed)) {
      output->period_sample_frames = parsed;
    } else if (name == "--capture-buffer-frames" &&
               ParseUnsigned(value, &parsed)) {
      output->capture_buffer_sample_frames = parsed;
    } else if (name == "--playback-buffer-frames" &&
               ParseUnsigned(value, &parsed)) {
      output->playback_buffer_sample_frames = parsed;
    } else {
      return false;
    }
  }
  return argc > 1 && (argc % 2) == 1 && !output->device_name.empty() &&
         output->periods > 0U && output->periods <= 500U &&
         (output->capture_channels == 2U ||
          output->capture_channels == 4U) &&
         output->playback_channels == 2U &&
         output->period_sample_frames == 960U &&
         output->capture_buffer_sample_frames >= 1920U &&
         output->capture_buffer_sample_frames <= 4096U &&
         output->playback_buffer_sample_frames >= 1920U &&
         output->playback_buffer_sample_frames <= 4096U;
}

struct AcceptedCounters final {
  std::uint32_t chunks{0U};
  std::uint32_t sample_frames{0U};
  std::uint32_t end_of_stream_chunks{0U};
};

bool DrainAccepted(AcceptedRenderQueue* const queue,
                   AcceptedCounters* const counters) noexcept {
  if (queue == nullptr || counters == nullptr) {
    return false;
  }
  while (true) {
    auto read = queue->TryAcquireRead();
    if (!read) {
      return true;
    }
    const auto& chunk = read.frame();
    if (!chunk.HasValidLength()) {
      return false;
    }
    ++counters->chunks;
    counters->sample_frames += chunk.accepted_samples;
    if (chunk.source_end_of_stream) {
      ++counters->end_of_stream_chunks;
    }
    if (!read.Release()) {
      return false;
    }
  }
}

PlaybackGainLimiterConfig UnityGainConfig() {
  PlaybackGainLimiterConfig config{};
  config.volume_percent = 60U;
  config.speaker_gain_percent = 100U;
  config.duck_target_percent = 25U;
  config.duck_ramp_ms = 80U;
  config.recovery_ramp_ms = 80U;
  config.limiter_ceiling_percent = 95U;
  config.limiter_release_ms = 80U;
  return config;
}

TtsPcmFrame24k SilenceFrame(const PlaybackGeneration& generation,
                            const std::uint32_t sequence,
                            const std::uint64_t timestamp_us,
                            const bool end_of_stream) {
  TtsPcmFrame24k frame{};
  frame.format = {24000U, 20U, 1U, boompi::audio::SampleFormat::kPcmS16Le};
  frame.metadata.monotonic_timestamp_us = timestamp_us;
  frame.metadata.sequence = sequence;
  frame.metadata.epoch = generation.epoch;
  frame.metadata.turn_id = generation.turn_id;
  frame.metadata.stream_id = generation.stream_id;
  frame.valid_source_samples = TtsPcmFrame24k::kFrameSamples;
  frame.end_of_stream = end_of_stream;
  frame.samples.fill(0);
  return frame;
}

}  // namespace

int main(const int argc, char** const argv) {
  Options options;
  if (!ParseOptions(argc, argv, &options)) {
    std::cerr << "usage: boompi-alsa-smoke --device <pcm> --periods <1..500> "
                 "--capture-channels <2|4> --playback-channels 2 "
                 "--period-frames 960 --capture-buffer-frames <1920..4096> "
                 "--playback-buffer-frames <1920..4096>\n";
    return 2;
  }

  AlsaPcmStream capture_stream;
  AlsaPcmStream playback_stream;
  AlsaPcmStreamConfig capture_config{};
  capture_config.device_name = options.device_name;
  capture_config.direction = AlsaPcmDirection::kCapture;
  capture_config.channels = options.capture_channels;
  capture_config.period_sample_frames = options.period_sample_frames;
  capture_config.buffer_sample_frames =
      options.capture_buffer_sample_frames;
  boompi::Status status = AlsaPcmStream::Open(capture_config, &capture_stream);
  if (!status.ok()) {
    std::cerr << "capture_open_failed=" << status.message() << '\n';
    return 3;
  }

  AlsaPcmStreamConfig playback_config{};
  playback_config.device_name = options.device_name;
  playback_config.direction = AlsaPcmDirection::kPlayback;
  playback_config.channels = options.playback_channels;
  playback_config.period_sample_frames = options.period_sample_frames;
  playback_config.buffer_sample_frames =
      options.playback_buffer_sample_frames;
  status = AlsaPcmStream::Open(playback_config, &playback_stream);
  if (!status.ok()) {
    std::cerr << "playback_open_failed=" << status.message() << '\n';
    return 4;
  }

  SteadyAudioIoClock clock;
  AudioPeriodTransferConfig transfer_config{10U, 250U};
  CaptureEpochFence capture_fence;
  boompi::audio::PlaybackEpochFence playback_fence;
  if (!capture_fence.AdvanceTo(1U).ok() ||
      !playback_fence.AdvanceTo(1U).ok()) {
    std::cerr << "epoch_fence_initialization_failed\n";
    return 5;
  }

  CapturePeriodReader48k capture_reader(&capture_stream, &clock);
  if (!capture_reader.Configure(transfer_config,
                                options.capture_channels).ok() ||
      !capture_reader.Activate(1U, 1U, capture_fence).ok()) {
    std::cerr << "capture_reader_initialization_failed\n";
    return 6;
  }

  PcmPlaybackSink48k playback_sink(&playback_stream, &clock);
  AcceptedRenderQueue accepted_queue;
  PlaybackCommitter playback_committer;
  const PlaybackGeneration playback_generation{1U, 1U, 1U};
  if (!PlaybackCommitter::Create(PlaybackCommitterConfig{}, &playback_fence,
                                 &playback_sink, &accepted_queue,
                                 &playback_committer)
           .ok() ||
      !playback_committer.Initialize().ok() ||
      !playback_committer.Arm(playback_generation).ok()) {
    std::cerr << "playback_committer_initialization_failed\n";
    return 7;
  }

  PlaybackRenderer24To48 renderer;
  if (!PlaybackRenderer24To48::Create(UnityGainConfig(), &renderer).ok() ||
      !renderer.Arm(playback_generation).ok()) {
    std::cerr << "playback_renderer_initialization_failed\n";
    return 8;
  }

  AudioIoStopFlag stop_flag;
  std::atomic<std::uint32_t> capture_frames{0U};
  std::atomic<std::uint32_t> playback_chunks{0U};
  std::atomic<std::uint32_t> worker_failures{0U};
  std::atomic<std::uint32_t> capture_failure_code{0U};
  std::atomic<std::uint32_t> playback_failure_code{0U};
  AcceptedCounters accepted_counters;

  std::thread capture_worker([&]() {
    boompi::audio::CaptureFrame frame{};
    for (std::uint32_t period = 0U; period < options.periods; ++period) {
      const auto result =
          capture_reader.Read(0U, capture_fence, stop_flag, &frame);
      if (!result.captured()) {
        capture_failure_code.store(
            static_cast<std::uint32_t>(result.code),
            std::memory_order_relaxed);
        worker_failures.fetch_add(1U, std::memory_order_relaxed);
        stop_flag.RequestStop();
        return;
      }
      capture_frames.fetch_add(1U, std::memory_order_relaxed);
    }
  });

  std::thread playback_worker([&]() {
    const auto commit_rendered = [&](const PlaybackPcmFrame48k& frame) {
      if (!playback_committer.Submit(frame).accepted()) {
        return false;
      }
      const std::uint64_t started_us = clock.NowUs();
      while (!stop_flag.stop_requested() &&
             clock.NowUs() - started_us <= 250000U) {
        const auto commit = playback_committer.PumpOnce();
        if (!DrainAccepted(&accepted_queue, &accepted_counters)) {
          return false;
        }
        if (commit.chunk_completed) {
          return true;
        }
        if (commit.code == PlaybackCommitCode::kAcceptedPartial) {
          continue;
        }
        if (commit.code != PlaybackCommitCode::kWouldBlock &&
            commit.code != PlaybackCommitCode::kInterrupted) {
          playback_failure_code.store(
              400U + static_cast<std::uint32_t>(commit.code),
              std::memory_order_relaxed);
          return false;
        }
        const auto wait = playback_stream.Wait(10U);
        if (wait.code != PcmBackendCode::kReady &&
            wait.code != PcmBackendCode::kTimeout &&
            wait.code != PcmBackendCode::kInterrupted) {
          playback_failure_code.store(
              500U + static_cast<std::uint32_t>(wait.code),
              std::memory_order_relaxed);
          return false;
        }
      }
      playback_failure_code.store(600U, std::memory_order_relaxed);
      return false;
    };

    PlaybackPcmFrame48k rendered{};
    for (std::uint32_t period = 0U; period < options.periods; ++period) {
      const bool last_period = period + 1U == options.periods;
      const auto input = SilenceFrame(
          playback_generation, period,
          clock.NowUs() + static_cast<std::uint64_t>(period) * 20000U,
          last_period);
      const auto render_result = renderer.Process(input, &rendered);
      if (!render_result.produced() || !commit_rendered(rendered)) {
        const std::uint32_t commit_failure =
            playback_failure_code.load(std::memory_order_relaxed);
        playback_failure_code.store(
            render_result.produced()
                ? (commit_failure == 0U ? 100U : commit_failure)
                : 200U + static_cast<std::uint32_t>(render_result.code),
            std::memory_order_relaxed);
        worker_failures.fetch_add(1U, std::memory_order_relaxed);
        stop_flag.RequestStop();
        return;
      }
      playback_chunks.fetch_add(1U, std::memory_order_relaxed);
      if (last_period) {
        if (!render_result.drain_required ||
            !renderer.Drain(&rendered).produced() ||
            !commit_rendered(rendered)) {
          playback_failure_code.store(300U, std::memory_order_relaxed);
          worker_failures.fetch_add(1U, std::memory_order_relaxed);
          stop_flag.RequestStop();
          return;
        }
        playback_chunks.fetch_add(1U, std::memory_order_relaxed);
      }
    }
  });

  capture_worker.join();
  playback_worker.join();

  const std::uint32_t failures =
      worker_failures.load(std::memory_order_relaxed);
  const auto& capture_actual = capture_stream.actual_config();
  const auto& playback_actual = playback_stream.actual_config();
  std::cout << "capture_rate_hz=" << capture_actual.sample_rate_hz
            << " capture_channels="
            << static_cast<unsigned int>(capture_actual.channels)
            << " capture_period_frames="
            << capture_actual.period_sample_frames
            << " capture_buffer_frames="
            << capture_actual.buffer_sample_frames
            << " playback_rate_hz=" << playback_actual.sample_rate_hz
            << " playback_channels="
            << static_cast<unsigned int>(playback_actual.channels)
            << " playback_period_frames="
            << playback_actual.period_sample_frames
            << " playback_buffer_frames="
            << playback_actual.buffer_sample_frames
            << " captured_periods="
            << capture_frames.load(std::memory_order_relaxed)
            << " rendered_chunks="
            << playback_chunks.load(std::memory_order_relaxed)
            << " accepted_chunks=" << accepted_counters.chunks
            << " accepted_frames=" << accepted_counters.sample_frames
            << " eos_chunks=" << accepted_counters.end_of_stream_chunks
            << " capture_failure_code="
            << capture_failure_code.load(std::memory_order_relaxed)
            << " playback_failure_code="
            << playback_failure_code.load(std::memory_order_relaxed)
            << " failures=" << failures << '\n';
  return failures == 0U &&
                 capture_frames.load(std::memory_order_relaxed) ==
                     options.periods &&
                 playback_chunks.load(std::memory_order_relaxed) ==
                     options.periods + 1U &&
                 accepted_counters.end_of_stream_chunks == 1U
             ? 0
             : 9;
}
