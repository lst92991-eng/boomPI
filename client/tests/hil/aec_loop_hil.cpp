/** @file 有界播放样本回归：绕过唤醒和网络，复用生产音频链测量自激候选。 */
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

#include "boompi/audio/audio_engine.h"
#include "boompi/config/voice_client_config.h"

#ifndef BOOMPI_AEC_LOOP_ACTIVE_PROBE
#define BOOMPI_AEC_LOOP_ACTIVE_PROBE 0
#endif

namespace {

constexpr std::size_t kTtsFrameBytes = 480U * sizeof(std::int16_t);
constexpr std::size_t kMaximumTtsFrames = 70U;  // 1.4 s，低于生产 1.5 s 环容量。
constexpr unsigned kSettleFrames = 250U;
constexpr unsigned kPostPlaybackFrames = 50U;
constexpr unsigned kPreadaptRepeats = 4U;
constexpr unsigned kPlaybackRepeats = 6U;
constexpr unsigned kPreadaptGuardFrames = 1U;  // 对齐 3A 固定的 20 ms 输出启动延迟。
constexpr unsigned kMaximumCaptureFrames = 500U;
constexpr unsigned kBargeConfirmFrames = 6U;
constexpr unsigned kFollowUpConfirmFrames = 20U;
constexpr unsigned kBargeReferenceLowFrames = 3U;
constexpr unsigned kBargeReferenceWaitFrames = 15U;
constexpr unsigned kBargeEchoClearFrames = 10U;
constexpr std::size_t kPrequeueFrames = 8U;
constexpr int kCorrelationStrideSamples = 4;
constexpr double kDbfsFloor = -120.0;
using SignalSet = std::array<std::vector<std::int16_t>, 5U>;
enum class ProbeState : std::uint8_t { kIdle, kWaitReferenceLow, kClear, kVerify };

struct Metrics final {
  std::uint64_t squared_sum{0U};
  std::uint64_t sample_count{0U};
  std::uint32_t playback_frames{0U};
  std::uint32_t vad_frames{0U};
  std::uint32_t near_frames{0U};
  std::uint32_t discontinuities{0U};
  unsigned vad_run{0U};
  unsigned near_run{0U};
  unsigned maximum_vad_run{0U};
  unsigned maximum_near_run{0U};
  unsigned post_vad_run{0U};
  unsigned post_near_run{0U};
  unsigned maximum_post_vad_run{0U};
  unsigned maximum_post_near_run{0U};
  std::uint32_t reference_frames{0U};
  std::uint32_t post_vad_frames{0U};
  std::uint32_t post_near_frames{0U};
  int first_reference_frame{-1};
  int first_near_ms{-1};
  int peak{0};
  bool post_vad_started{false};
  SignalSet signals{};
};

struct Correlation final {
  double value{0.0};
  int lag_samples{0};
};

double RmsDbfs(const std::vector<std::int16_t>& signal) {
  if (signal.empty()) return kDbfsFloor;
  long double squared_sum = 0.0;
  for (const std::int16_t sample : signal) {
    const long double value = sample;
    squared_sum += value * value;
  }
  if (squared_sum == 0.0) return kDbfsFloor;
  const double rms = std::sqrt(static_cast<double>(
      squared_sum / static_cast<long double>(signal.size())));
  return 20.0 * std::log10(rms / 32768.0);
}

double Mean(const std::vector<std::int16_t>& signal) {
  if (signal.empty()) return 0.0;
  long double sum = 0.0;
  for (const std::int16_t sample : signal) sum += sample;
  return static_cast<double>(sum / static_cast<long double>(signal.size()));
}

double AcRmsDbfs(const std::vector<std::int16_t>& signal) {
  if (signal.empty()) return kDbfsFloor;
  const long double mean = Mean(signal);
  long double squared_sum = 0.0;
  for (const std::int16_t sample : signal) {
    const long double value = static_cast<long double>(sample) - mean;
    squared_sum += value * value;
  }
  if (squared_sum == 0.0) return kDbfsFloor;
  const double rms = std::sqrt(static_cast<double>(
      squared_sum / static_cast<long double>(signal.size())));
  return 20.0 * std::log10(rms / 32768.0);
}

Correlation BestCorrelation(const std::vector<std::int16_t>& reference,
                            const std::vector<std::int16_t>& response,
                            const int maximum_lag_samples) {
  Correlation best{};
  if (reference.size() != response.size() || reference.empty()) return best;
  const long double reference_mean = Mean(reference);
  const long double response_mean = Mean(response);
  for (int lag = 0; lag <= maximum_lag_samples;
       lag += kCorrelationStrideSamples) {
    long double cross = 0.0, reference_energy = 0.0, response_energy = 0.0;
    const std::size_t offset = static_cast<std::size_t>(lag);
    for (std::size_t i = 0U; i + offset < reference.size();
         i += static_cast<std::size_t>(kCorrelationStrideSamples)) {
      const long double x =
          static_cast<long double>(reference[i]) - reference_mean;
      const long double y =
          static_cast<long double>(response[i + offset]) - response_mean;
      cross += x * y;
      reference_energy += x * x;
      response_energy += y * y;
    }
    if (reference_energy == 0.0 || response_energy == 0.0) continue;
    const double correlation = static_cast<double>(
        cross / std::sqrt(reference_energy * response_energy));
    if (std::abs(correlation) > std::abs(best.value)) {
      best.value = correlation;
      best.lag_samples = lag;
    }
  }
  return best;
}

void AppendSignals(const boompi::audio::CaptureFrame& frame,
                   SignalSet* const signals) {
#ifdef BOOMPI_AEC_LOOP_DIAGNOSTICS
  for (std::size_t channel = 0U; channel < frame.aec_input.size(); ++channel) {
    (*signals)[channel].insert((*signals)[channel].end(),
                               frame.aec_input[channel].begin(),
                               frame.aec_input[channel].end());
  }
  (*signals)[4U].insert((*signals)[4U].end(), frame.pcm.begin(),
                        frame.pcm.end());
#else
  static_cast<void>(frame);
  static_cast<void>(signals);
#endif
}

bool ReadFixture(const char* const path, std::vector<std::uint8_t>* output) {
  if (path == nullptr || output == nullptr) return false;
  std::ifstream input(path, std::ios::binary | std::ios::ate);
  if (!input) return false;
  const std::streamoff size = input.tellg();
  const std::streamoff maximum =
      static_cast<std::streamoff>(kMaximumTtsFrames * kTtsFrameBytes);
  if (size <= 0 || size > maximum || size % kTtsFrameBytes != 0) return false;
  output->resize(static_cast<std::size_t>(size));
  input.seekg(0, std::ios::beg);
  return input.read(reinterpret_cast<char*>(output->data()), size).good();
}

bool HasReference(const boompi::audio::CaptureFrame& frame) {
#ifdef BOOMPI_AEC_LOOP_DIAGNOSTICS
  for (std::size_t channel = 2U; channel < 4U; ++channel) {
    for (const std::int16_t sample : frame.aec_input[channel]) {
      if (sample > 64 || sample < -64) return true;
    }
  }
#endif
  return false;
}

void ObservePlayback(const boompi::audio::CaptureFrame& frame,
                     const std::uint32_t playback_frame,
                     Metrics* const metrics) {
  if (frame.discontinuity) ++metrics->discontinuities;
  ++metrics->playback_frames;
  if (HasReference(frame)) {
    ++metrics->reference_frames;
    if (metrics->first_reference_frame < 0) {
      metrics->first_reference_frame = static_cast<int>(playback_frame);
    }
  }
  metrics->vad_run = frame.vad_now ? metrics->vad_run + 1U : 0U;
  metrics->near_run = frame.near_voice ? metrics->near_run + 1U : 0U;
  metrics->maximum_vad_run = std::max(metrics->maximum_vad_run,
                                      metrics->vad_run);
  metrics->maximum_near_run = std::max(metrics->maximum_near_run,
                                       metrics->near_run);
  if (frame.vad_now) ++metrics->vad_frames;
  if (frame.near_voice) {
    ++metrics->near_frames;
    if (metrics->first_near_ms < 0 && metrics->first_reference_frame >= 0) {
      metrics->first_near_ms =
          (static_cast<int>(playback_frame) - metrics->first_reference_frame) *
          20;
    }
  }
  for (const std::int16_t sample : frame.pcm) {
    const int value = sample;
    metrics->peak = std::max(metrics->peak, std::abs(value));
    metrics->squared_sum += static_cast<std::uint64_t>(
        static_cast<std::int64_t>(value) * value);
    ++metrics->sample_count;
  }
  AppendSignals(frame, &metrics->signals);
}

void ObservePostPlayback(const boompi::audio::CaptureFrame& frame,
                         Metrics* const metrics) {
  if (frame.discontinuity) ++metrics->discontinuities;
  metrics->post_vad_run = frame.vad_now ? metrics->post_vad_run + 1U : 0U;
  metrics->post_near_run =
      frame.near_voice ? metrics->post_near_run + 1U : 0U;
  metrics->maximum_post_vad_run =
      std::max(metrics->maximum_post_vad_run, metrics->post_vad_run);
  metrics->maximum_post_near_run =
      std::max(metrics->maximum_post_near_run, metrics->post_near_run);
  if (frame.vad_now) ++metrics->post_vad_frames;
  if (frame.near_voice) ++metrics->post_near_frames;
  metrics->post_vad_started = metrics->post_vad_started || frame.vad_started;
}

}  // namespace

int main(int argc, char* argv[]) {
  if (argc != 2) {
    std::cerr << "usage: boompi-aec-loop-hil <24k-mono-s16le.raw>\n";
    return EXIT_FAILURE;
  }

  std::vector<std::uint8_t> fixture;
  if (!ReadFixture(argv[1], &fixture)) {
    std::cerr << "boompi-aec-loop-hil: fixture must be 20 ms aligned, nonempty, and <= 1.4 s\n";
    return EXIT_FAILURE;
  }

  boompi::config::VoiceClientConfig client_config;
  const boompi::Status loaded =
      boompi::config::LoadVoiceClientConfigFromEnvironment(&client_config);
  if (!loaded.ok()) {
    std::cerr << "boompi-aec-loop-hil: configuration failed: "
              << loaded.message() << '\n';
    return EXIT_FAILURE;
  }

  boompi::audio::AudioEngineConfig audio_config{};
  audio_config.capture_pcm = client_config.capture_pcm;
  audio_config.playback_pcm = client_config.playback_pcm;
  audio_config.snowboy_resource = client_config.snowboy_resource_path;
  audio_config.snowboy_model = client_config.snowboy_model_path;
  audio_config.snowboy_sensitivity = client_config.snowboy_sensitivity;
  audio_config.playback_gain =
      static_cast<float>(client_config.volume_percent) *
      client_config.speaker_gain_percent / 10000.0F;
  audio_config.left_polarity = client_config.capture_left_polarity;
  audio_config.right_polarity = client_config.capture_right_polarity;

  boompi::audio::AudioEngine audio;
  if (!audio.Open(audio_config)) {
    std::cerr << "boompi-aec-loop-hil: audio open failed: "
              << audio.last_error() << '\n';
    return EXIT_FAILURE;
  }

  boompi::audio::CaptureFrame frame{};
  Metrics metrics{};
  SignalSet quiet_signals{};
  for (unsigned index = 0U; index < kSettleFrames; ++index) {
    if (!audio.Capture(&frame)) {
      std::cerr << "boompi-aec-loop-hil: settle capture failed: "
                << audio.last_error() << '\n';
      return EXIT_FAILURE;
    }
    if (frame.discontinuity) {
      std::cerr << "boompi-aec-loop-hil: capture discontinuity during settle\n";
      return EXIT_FAILURE;
    }
    AppendSignals(frame, &quiet_signals);
  }
  if (!audio.BeginPlayback()) {
    std::cerr << "boompi-aec-loop-hil: playback start failed: "
              << audio.last_error() << '\n';
    return EXIT_FAILURE;
  }
  const std::size_t fixture_frames = fixture.size() / kTtsFrameBytes;
  const std::size_t playback_frames = fixture_frames * kPlaybackRepeats;
  std::size_t next_fixture_frame = 0U;
  const std::size_t initial_frames =
      std::min(playback_frames, kPrequeueFrames);
  for (; next_fixture_frame < initial_frames; ++next_fixture_frame) {
    if (!audio.QueueTts24k(
                           fixture.data() +
                               (next_fixture_frame % fixture_frames) * kTtsFrameBytes,
                           kTtsFrameBytes, next_fixture_frame)) {
      std::cerr << "boompi-aec-loop-hil: fixture queue failed: "
                << audio.last_error() << '\n';
      return EXIT_FAILURE;
    }
  }
  bool playback_ending = next_fixture_frame == playback_frames;
  if (playback_ending && !audio.EndPlayback()) {
    std::cerr << "boompi-aec-loop-hil: playback end failed: "
              << audio.last_error() << '\n';
    return EXIT_FAILURE;
  }

  bool saw_playback = false, saw_reference = false, probe_confirmed = false;
  bool completed_score_window = false, completed_post_window = false;
  unsigned post_frames = 0U, probe_frames = 0U, probe_near_frames = 0U;
  unsigned probe_attempts = 0U;
  ProbeState probe_state = ProbeState::kIdle;
  std::uint32_t reference_timeline_frames = 0U, score_frame = 0U;
  for (unsigned index = 0U; index < kMaximumCaptureFrames; ++index) {
    if (next_fixture_frame < playback_frames) {
      if (!audio.QueueTts24k(
              fixture.data() +
                  (next_fixture_frame % fixture_frames) * kTtsFrameBytes,
              kTtsFrameBytes, next_fixture_frame)) {
        std::cerr << "boompi-aec-loop-hil: streaming fixture queue failed: "
                  << audio.last_error() << '\n';
        return EXIT_FAILURE;
      }
      ++next_fixture_frame;
      if (next_fixture_frame == playback_frames) {
        playback_ending = true;
        if (!audio.EndPlayback()) {
          std::cerr << "boompi-aec-loop-hil: playback end failed: "
                    << audio.last_error() << '\n';
          return EXIT_FAILURE;
        }
      }
    }
    const bool playing_before_capture = audio.playing();
    if (!audio.Capture(&frame)) {
      std::cerr << "boompi-aec-loop-hil: capture failed: "
                << audio.last_error() << '\n';
      return EXIT_FAILURE;
    }
    if (frame.discontinuity) {
      std::cerr << "boompi-aec-loop-hil: capture discontinuity invalidated preadaptation\n";
      return EXIT_FAILURE;
    }
    const bool playing = playing_before_capture || audio.playing();
    saw_playback = saw_playback || playing;
    if constexpr (BOOMPI_AEC_LOOP_ACTIVE_PROBE != 0) {
      if (!audio.playback_done()) {
        if (probe_state == ProbeState::kIdle && frame.near_voice) {
          ++probe_attempts; probe_frames = probe_near_frames = 0U;
          probe_state = ProbeState::kWaitReferenceLow; audio.Duck(true);
        } else if (probe_state == ProbeState::kWaitReferenceLow) {
          if (++probe_frames > kBargeReferenceWaitFrames) {
            probe_frames = probe_near_frames = 0U; probe_state = ProbeState::kIdle;
            audio.Duck(false);
          } else {
            probe_near_frames = frame.reference_active ? 0U : probe_near_frames + 1U;
            if (probe_near_frames >= kBargeReferenceLowFrames) {
              probe_frames = probe_near_frames = 0U; probe_state = ProbeState::kClear;
            }
          }
        } else if (probe_state == ProbeState::kClear) {
          if (frame.reference_active) {
            probe_frames = probe_near_frames = 0U; probe_state = ProbeState::kIdle;
            audio.Duck(false);
          } else if (++probe_frames >= kBargeEchoClearFrames) {
            probe_frames = probe_near_frames = 0U; probe_state = ProbeState::kVerify;
            if (!audio.ResetListener()) {
              std::cerr << "boompi-aec-loop-hil: probe VAD reset failed\n";
              return EXIT_FAILURE;
            }
          }
        } else if (frame.reference_active || !frame.near_voice) {
          probe_frames = probe_near_frames = 0U; probe_state = ProbeState::kIdle;
          audio.Duck(false);
        } else if (++probe_near_frames >= kBargeConfirmFrames) {
          probe_confirmed = true; probe_frames = probe_near_frames = 0U;
          probe_state = ProbeState::kIdle; audio.Duck(false);
        }
      } else if (probe_state != ProbeState::kIdle) {
        probe_frames = probe_near_frames = 0U; probe_state = ProbeState::kIdle;
        audio.Duck(false);
      }
    }
    if (playing) {
      if (!saw_reference && HasReference(frame)) saw_reference = true;
      if (saw_reference) {
        const std::uint32_t score_begin = static_cast<std::uint32_t>(
            fixture_frames * kPreadaptRepeats + kPreadaptGuardFrames);
        if (reference_timeline_frames >= score_begin &&
            score_frame < fixture_frames) {
          ObservePlayback(frame, score_frame, &metrics);
          ++score_frame;
          completed_score_window = score_frame == fixture_frames;
        }
        ++reference_timeline_frames;
      }
      post_frames = 0U;
    } else if (saw_playback) {
      ObservePostPlayback(frame, &metrics);
      if (++post_frames >= kPostPlaybackFrames) {
        completed_post_window = true;
        break;
      }
    }
  }

  const bool playback_failed = audio.playback_failed();
  const std::string audio_error = audio.last_error();
  audio.Close();
  if (!saw_playback || !saw_reference || !playback_ending ||
      !completed_score_window || !completed_post_window ||
      playback_failed || metrics.playback_frames == 0U ||
      metrics.reference_frames == 0U) {
    std::cerr << "boompi-aec-loop-hil: playback failed or was not observed: "
              << audio_error << '\n';
    return EXIT_FAILURE;
  }

  double rms_dbfs = kDbfsFloor;
  if (metrics.squared_sum != 0U && metrics.sample_count != 0U) {
    const double rms = std::sqrt(static_cast<double>(metrics.squared_sum) /
                                 static_cast<double>(metrics.sample_count));
    rms_dbfs = 20.0 * std::log10(rms / 32768.0);
  }
  const bool would_barge = BOOMPI_AEC_LOOP_ACTIVE_PROBE != 0
      ? probe_confirmed : metrics.maximum_near_run >= kBargeConfirmFrames;
  const bool would_follow_up =
      metrics.maximum_post_near_run >= kFollowUpConfirmFrames;
  const Correlation mic_left =
      BestCorrelation(metrics.signals[2U], metrics.signals[0U], 800);
  const Correlation mic_right =
      BestCorrelation(metrics.signals[2U], metrics.signals[1U], 800);
  const Correlation processed =
      BestCorrelation(metrics.signals[2U], metrics.signals[4U], 1200);
  const Correlation reference_right =
      BestCorrelation(metrics.signals[2U], metrics.signals[3U], 32);
  std::cout << "AEC_LOOP_RESULT {\"reference_channels\":"
            << BOOMPI_ROCKCHIP_REFERENCE_CHANNELS
            << ",\"active_probe\":" << BOOMPI_AEC_LOOP_ACTIVE_PROBE
            << ",\"aec_metrics_valid\":"
            << (BOOMPI_AEC_LOOP_ACTIVE_PROBE == 0 ? "true" : "false")
            << ",\"stdt_enabled\":" << BOOMPI_ROCKCHIP_ENABLE_STDT
            << ",\"delay_samples\":" << BOOMPI_ROCKCHIP_DELAY_SAMPLES
            << ",\"fixture_frames\":" << fixture_frames
            << ",\"preadapt_repeats\":" << kPreadaptRepeats
            << ",\"playback_frames\":" << metrics.playback_frames
            << ",\"reference_frames\":" << metrics.reference_frames
            << ",\"vad_frames\":" << metrics.vad_frames
            << ",\"near_frames\":" << metrics.near_frames
            << ",\"probe_attempts\":" << probe_attempts
            << ",\"probe_confirmed\":" << (probe_confirmed ? "true" : "false")
            << ",\"max_vad_run\":" << metrics.maximum_vad_run
            << ",\"max_near_run\":" << metrics.maximum_near_run
            << ",\"first_near_ms\":" << metrics.first_near_ms
            << ",\"post_vad_frames\":" << metrics.post_vad_frames
            << ",\"post_near_frames\":" << metrics.post_near_frames
            << ",\"max_post_vad_run\":" << metrics.maximum_post_vad_run
            << ",\"max_post_near_run\":" << metrics.maximum_post_near_run
            << ",\"processed_peak\":" << metrics.peak
            << ",\"processed_rms_dbfs\":" << rms_dbfs
            << ",\"quiet_mic_left_mean\":" << Mean(quiet_signals[0U])
            << ",\"quiet_mic_right_mean\":" << Mean(quiet_signals[1U])
            << ",\"quiet_mic_left_ac_rms_dbfs\":"
            << AcRmsDbfs(quiet_signals[0U])
            << ",\"quiet_mic_right_ac_rms_dbfs\":"
            << AcRmsDbfs(quiet_signals[1U])
            << ",\"quiet_processed_mean\":" << Mean(quiet_signals[4U])
            << ",\"quiet_processed_ac_rms_dbfs\":"
            << AcRmsDbfs(quiet_signals[4U])
            << ",\"mic_left_mean\":" << Mean(metrics.signals[0U])
            << ",\"mic_right_mean\":" << Mean(metrics.signals[1U])
            << ",\"mic_left_rms_dbfs\":" << RmsDbfs(metrics.signals[0U])
            << ",\"mic_right_rms_dbfs\":" << RmsDbfs(metrics.signals[1U])
            << ",\"mic_left_ac_rms_dbfs\":"
            << AcRmsDbfs(metrics.signals[0U])
            << ",\"mic_right_ac_rms_dbfs\":"
            << AcRmsDbfs(metrics.signals[1U])
            << ",\"ref_left_rms_dbfs\":" << RmsDbfs(metrics.signals[2U])
            << ",\"ref_right_rms_dbfs\":" << RmsDbfs(metrics.signals[3U])
            << ",\"processed_mean\":" << Mean(metrics.signals[4U])
            << ",\"processed_ac_rms_dbfs\":"
            << AcRmsDbfs(metrics.signals[4U])
            << ",\"ref_lr_corr\":" << reference_right.value
            << ",\"mic_left_ref_corr\":" << mic_left.value
            << ",\"mic_left_ref_lag_ms\":"
            << mic_left.lag_samples / 16.0
            << ",\"mic_right_ref_corr\":" << mic_right.value
            << ",\"mic_right_ref_lag_ms\":"
            << mic_right.lag_samples / 16.0
            << ",\"processed_ref_corr\":" << processed.value
            << ",\"processed_ref_lag_ms\":"
            << processed.lag_samples / 16.0
            << ",\"capture_discontinuities\":" << metrics.discontinuities
            << ",\"would_barge\":" << (would_barge ? "true" : "false")
            << ",\"would_follow_up\":"
            << (would_follow_up ? "true" : "false")
            << "}\n";
  return EXIT_SUCCESS;
}
