#include <cstdint>
#include <cstdio>

#include "boompi/platform/rv1106/rockchip_3a_audio_dsp_adapter.h"

namespace {

constexpr std::uint32_t kFirstEpoch = 100U;
constexpr std::uint32_t kFirstStreamId = 200U;
constexpr std::uint32_t kInputFramesPerRun = 4U;
constexpr std::uint32_t kExpectedBlocksPerRun = 5U;

boompi::audio::AudioDspEngineConfig ProbeConfig() {
  return {boompi::audio::AudioDspReferenceSource::kHardwareCapture,
          boompi::audio::AudioDspReferenceLayout::kStereo,
          boompi::audio::kBoompiV1AudioDspFeatures};
}

boompi::audio::DspCaptureFrame16k MakeZeroInput(
    const std::uint32_t epoch,
    const std::uint32_t stream_id,
    const std::uint32_t sequence) {
  boompi::audio::DspCaptureFrame16k frame{};
  frame.metadata.monotonic_timestamp_us =
      static_cast<std::uint64_t>(sequence) * 20000U;
  frame.metadata.sequence = sequence;
  frame.metadata.stream_id = stream_id;
  frame.metadata.turn_id = 0U;
  frame.metadata.epoch = epoch;
  return frame;
}

struct ProbeRunResult final {
  std::uint32_t processed_blocks{0U};
  std::uint32_t output_frames{0U};
  bool metadata_valid{true};
  bool frame_valid{true};
};

ProbeRunResult RunOnce(
    boompi::platform::rv1106::Rockchip3aAudioDspAdapter* const adapter,
    const std::uint32_t epoch,
    const std::uint32_t stream_id) {
  ProbeRunResult run{};
  for (std::uint32_t sequence = 0U; sequence < kInputFramesPerRun;
       ++sequence) {
    const auto process_result =
        adapter->Process(MakeZeroInput(epoch, stream_id, sequence));
    if (process_result.code !=
            boompi::audio::AudioDspFrameBridgeCode::kNeedMoreInput &&
        process_result.code !=
            boompi::audio::AudioDspFrameBridgeCode::kOutputAvailable) {
      run.frame_valid = false;
      return run;
    }
    run.processed_blocks += process_result.processed_blocks;

    std::uint32_t available = process_result.available_outputs;
    while (available != 0U) {
      boompi::audio::Mono16kFrame output{};
      const auto pop_result = adapter->TryPop(&output);
      if (!pop_result.output_available() || !output.HasValidLength()) {
        run.frame_valid = false;
        return run;
      }
      if (output.metadata.epoch != epoch ||
          output.metadata.stream_id != stream_id ||
          output.metadata.sequence != run.output_frames) {
        run.metadata_valid = false;
      }
      for (const std::int16_t sample : output.samples) {
        if (sample != 0) {
          run.frame_valid = false;
        }
      }
      ++run.output_frames;
      available = pop_result.available_outputs;
    }
  }
  return run;
}

bool RunSucceeded(const ProbeRunResult& result) {
  return result.processed_blocks == kExpectedBlocksPerRun &&
         result.output_frames == kInputFramesPerRun &&
         result.metadata_valid && result.frame_valid;
}

}  // namespace

int main() {
  boompi::platform::rv1106::Rockchip3aPreprocessSession session;
  boompi::platform::rv1106::Rockchip3aAudioDspAdapter adapter;
  const boompi::Status create_status =
      boompi::platform::rv1106::Rockchip3aAudioDspAdapter::Create(
          &session, ProbeConfig(), &adapter);
  const bool created = create_status.ok();

  bool first_armed = false;
  bool second_armed = false;
  ProbeRunResult first{};
  ProbeRunResult second{};
  if (created) {
    first_armed = adapter.Arm(kFirstEpoch, kFirstStreamId).ok();
    if (first_armed) {
      first = RunOnce(&adapter, kFirstEpoch, kFirstStreamId);
    }
    second_armed = adapter.Arm(kFirstEpoch + 1U,
                               kFirstStreamId + 1U)
                       .ok();
    if (second_armed) {
      second = RunOnce(&adapter, kFirstEpoch + 1U,
                       kFirstStreamId + 1U);
    }
  }

  const bool passed = created && first_armed && second_armed &&
                      RunSucceeded(first) && RunSucceeded(second);
  std::printf(
      "{\"schema_version\":1,\"probe\":\"rockchip_3a_adapter\","
      "\"input_frames_per_run\":%u,\"first_processed_blocks\":%u,"
      "\"first_output_frames\":%u,\"second_processed_blocks\":%u,"
      "\"second_output_frames\":%u,\"metadata_valid\":%s,"
      "\"reinitialize_ok\":%s,\"status\":\"%s\"}\n",
      kInputFramesPerRun, first.processed_blocks, first.output_frames,
      second.processed_blocks, second.output_frames,
      first.metadata_valid && second.metadata_valid ? "true" : "false",
      second_armed && RunSucceeded(second) ? "true" : "false",
      passed ? "passed" : "failed");
  return passed ? 0 : 1;
}
