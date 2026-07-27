#include "boompi/audio/audio_dsp_frame_bridge.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>

#include "boompi/test/test_context.h"

#define BOOMPI_EXPECT(context, expression) \
  (context).Expect((expression), #expression, __FILE__, __LINE__)

namespace {

constexpr std::size_t kRecordedBlockCapacity = 8U;

std::int16_t SampleFor(const std::uint32_t sequence,
                       const std::size_t sample,
                       const std::size_t channel) {
  const std::uint64_t stream_sample =
      static_cast<std::uint64_t>(sequence) *
          boompi::audio::kSamplesPer16k20ms +
      sample;
  const std::uint64_t value =
      (stream_sample + static_cast<std::uint64_t>(channel) * 7001U) %
      30001U;
  return static_cast<std::int16_t>(static_cast<std::int32_t>(value) -
                                   15000);
}

boompi::audio::DspCaptureFrame16k MakeInput(
    const std::uint32_t epoch,
    const std::uint32_t stream_id,
    const std::uint32_t sequence,
    const bool discontinuity = false) {
  boompi::audio::DspCaptureFrame16k frame{};
  frame.metadata.monotonic_timestamp_us =
      1000000U + static_cast<std::uint64_t>(sequence) * 20000U;
  frame.metadata.sequence = sequence;
  frame.metadata.stream_id = stream_id;
  frame.metadata.turn_id = 77U + sequence;
  frame.metadata.epoch = epoch;
  frame.metadata.discontinuity = discontinuity;
  for (std::size_t channel = 0U;
       channel < boompi::audio::kDspCaptureChannelCount; ++channel) {
    for (std::size_t sample = 0U;
         sample < boompi::audio::kSamplesPer16k20ms; ++sample) {
      frame.planes[channel][sample] = SampleFor(sequence, sample, channel);
    }
  }
  return frame;
}

bool MetadataEqual(const boompi::audio::AudioFrameMetadata& left,
                   const boompi::audio::AudioFrameMetadata& right) {
  return left.monotonic_timestamp_us == right.monotonic_timestamp_us &&
         left.sequence == right.sequence &&
         left.stream_id == right.stream_id &&
         left.turn_id == right.turn_id && left.epoch == right.epoch &&
         left.discontinuity == right.discontinuity;
}

class CopyMicLeftBlockProcessor final
    : public boompi::audio::AudioDspBlockProcessor16k {
 public:
  boompi::audio::AudioDspBlockProcessResult Process(
      const boompi::audio::DspCapturePlanes16k16ms& input,
      boompi::audio::DspMonoBlock16k16ms* const output) noexcept override {
    ++call_count_;
    if (fail_on_call_ != 0U && call_count_ == fail_on_call_) {
      return {boompi::audio::AudioDspBlockProcessCode::kBackendFailure, 0U};
    }
    if (recorded_block_count_ < kRecordedBlockCapacity) {
      recorded_mic_left_[recorded_block_count_] = input[0U];
      ++recorded_block_count_;
    }
    if (output != nullptr) {
      std::copy(input[0U].begin(), input[0U].end(), output->begin());
    }
    return {boompi::audio::AudioDspBlockProcessCode::kProduced,
            reported_samples_};
  }

  void fail_on_call(const std::size_t call) noexcept { fail_on_call_ = call; }
  void report_samples(const std::uint32_t samples) noexcept {
    reported_samples_ = samples;
  }
  std::size_t call_count() const noexcept { return call_count_; }
  std::size_t recorded_block_count() const noexcept {
    return recorded_block_count_;
  }
  const boompi::audio::DspMonoBlock16k16ms& recorded_mic_left(
      const std::size_t block) const noexcept {
    return recorded_mic_left_[block];
  }

 private:
  std::array<boompi::audio::DspMonoBlock16k16ms,
             kRecordedBlockCapacity>
      recorded_mic_left_{};
  std::size_t call_count_{0U};
  std::size_t fail_on_call_{0U};
  std::size_t recorded_block_count_{0U};
  std::uint32_t reported_samples_{
      static_cast<std::uint32_t>(
          boompi::audio::kDspBlockSamplesPerChannel16k)};
};

void ExpectValidOutput(boompi::test::TestContext& context,
                       const boompi::audio::Mono16kFrame& output,
                       const boompi::audio::DspCaptureFrame16k& source) {
  BOOMPI_EXPECT(context, output.HasValidLength());
  BOOMPI_EXPECT(context, MetadataEqual(output.metadata, source.metadata));
  for (std::size_t sample = 0U;
       sample < boompi::audio::kSamplesPer16k20ms; ++sample) {
    BOOMPI_EXPECT(context,
                  output.samples[sample] == source.planes[0U][sample]);
  }
}

void TestCreateAndArmValidation(boompi::test::TestContext& context) {
  const boompi::audio::AudioDspBlockProcessResult unset_block_result{};
  BOOMPI_EXPECT(
      context,
      unset_block_result.code ==
          boompi::audio::AudioDspBlockProcessCode::kUnset);
  BOOMPI_EXPECT(context, !unset_block_result.produced_exact_block());
  const boompi::audio::AudioDspFrameBridgeResult unset_bridge_result{};
  BOOMPI_EXPECT(
      context,
      unset_bridge_result.code ==
          boompi::audio::AudioDspFrameBridgeCode::kUnset);
  BOOMPI_EXPECT(context, !unset_bridge_result.output_available());
  BOOMPI_EXPECT(context, !unset_bridge_result.needs_more_input());
  BOOMPI_EXPECT(context, !unset_bridge_result.requires_new_generation());

  CopyMicLeftBlockProcessor processor;
  boompi::audio::AudioDspFrameBridge16k bridge;
  const auto input = MakeInput(1U, 2U, 0U);
  auto result = bridge.Process(input);
  BOOMPI_EXPECT(context,
                result.code ==
                    boompi::audio::AudioDspFrameBridgeCode::kNotConfigured);

  boompi::audio::Mono16kFrame output{};
  BOOMPI_EXPECT(
      context,
      !boompi::audio::AudioDspFrameBridge16k::Create(nullptr, &bridge).ok());
  BOOMPI_EXPECT(
      context,
      !boompi::audio::AudioDspFrameBridge16k::Create(&processor, nullptr).ok());
  BOOMPI_EXPECT(
      context,
      boompi::audio::AudioDspFrameBridge16k::Create(&processor, &bridge).ok());
  BOOMPI_EXPECT(
      context,
      !boompi::audio::AudioDspFrameBridge16k::Create(&processor, &bridge).ok());
  BOOMPI_EXPECT(context, !bridge.Arm(0U, 1U).ok());
  BOOMPI_EXPECT(context, !bridge.Arm(1U, 0U).ok());
  BOOMPI_EXPECT(context, bridge.Arm(1U, 2U).ok());

  result = bridge.Process(input);
  BOOMPI_EXPECT(context, result.needs_more_input());
  BOOMPI_EXPECT(context, result.processed_blocks == 1U);
  BOOMPI_EXPECT(context, !bridge.Arm(1U, 3U).ok());
  BOOMPI_EXPECT(context, !bridge.Arm(0U, 3U).ok());

  result = bridge.Process(MakeInput(1U, 2U, 1U));
  BOOMPI_EXPECT(context, result.output_available());
  BOOMPI_EXPECT(context, result.available_outputs == 1U);
  result = bridge.TryPop(nullptr);
  BOOMPI_EXPECT(
      context,
      result.code == boompi::audio::AudioDspFrameBridgeCode::kInvalidOutput);
  BOOMPI_EXPECT(context, result.available_outputs == 1U);
  result = bridge.TryPop(&output);
  BOOMPI_EXPECT(context, result.output_available());
  ExpectValidOutput(context, output, input);
}

void TestExactFourFrameCadenceAndMetadata(
    boompi::test::TestContext& context) {
  CopyMicLeftBlockProcessor processor;
  boompi::audio::AudioDspFrameBridge16k bridge;
  BOOMPI_EXPECT(
      context,
      boompi::audio::AudioDspFrameBridge16k::Create(&processor, &bridge).ok());
  BOOMPI_EXPECT(context, bridge.Arm(5U, 9U).ok());

  constexpr std::array<std::uint32_t, 4U> kExpectedBlocks{{1U, 1U, 1U, 2U}};
  constexpr std::array<std::uint32_t, 4U> kExpectedOutputs{{0U, 1U, 1U, 2U}};
  std::uint32_t next_output_sequence = 0U;
  for (std::uint32_t sequence = 0U; sequence < 4U; ++sequence) {
    const auto input = MakeInput(5U, 9U, sequence);
    auto result = bridge.Process(input);
    BOOMPI_EXPECT(context,
                  result.processed_blocks == kExpectedBlocks[sequence]);
    BOOMPI_EXPECT(context,
                  result.available_outputs == kExpectedOutputs[sequence]);
    BOOMPI_EXPECT(
        context,
        result.code ==
            (kExpectedOutputs[sequence] == 0U
                 ? boompi::audio::AudioDspFrameBridgeCode::kNeedMoreInput
                 : boompi::audio::AudioDspFrameBridgeCode::kOutputAvailable));

    for (std::uint32_t output_index = 0U;
         output_index < kExpectedOutputs[sequence]; ++output_index) {
      boompi::audio::Mono16kFrame output{};
      result = bridge.TryPop(&output);
      BOOMPI_EXPECT(context, result.output_available());
      BOOMPI_EXPECT(
          context,
          result.available_outputs ==
              kExpectedOutputs[sequence] - output_index - 1U);
      ExpectValidOutput(
          context, output,
          MakeInput(5U, 9U, next_output_sequence));
      ++next_output_sequence;
    }
  }
  BOOMPI_EXPECT(context, next_output_sequence == 4U);
  BOOMPI_EXPECT(context, processor.call_count() == 5U);
  BOOMPI_EXPECT(context, processor.recorded_block_count() == 5U);

  for (std::size_t block = 0U; block < 5U; ++block) {
    for (std::size_t sample = 0U;
         sample < boompi::audio::kDspBlockSamplesPerChannel16k; ++sample) {
      const std::size_t stream_sample =
          block * boompi::audio::kDspBlockSamplesPerChannel16k + sample;
      const std::uint32_t source_sequence = static_cast<std::uint32_t>(
          stream_sample / boompi::audio::kSamplesPer16k20ms);
      const std::size_t source_sample =
          stream_sample % boompi::audio::kSamplesPer16k20ms;
      BOOMPI_EXPECT(
          context,
          processor.recorded_mic_left(block)[sample] ==
              SampleFor(source_sequence, source_sample, 0U));
    }
  }
}

void TestArmFlushesResidualAndQueuedOutput(
    boompi::test::TestContext& context) {
  CopyMicLeftBlockProcessor processor;
  boompi::audio::AudioDspFrameBridge16k bridge;
  BOOMPI_EXPECT(
      context,
      boompi::audio::AudioDspFrameBridge16k::Create(&processor, &bridge).ok());
  BOOMPI_EXPECT(context, bridge.Arm(15U, 16U).ok());
  for (std::uint32_t sequence = 0U; sequence < 4U; ++sequence) {
    bridge.Process(MakeInput(15U, 16U, sequence));
  }

  BOOMPI_EXPECT(context, bridge.Arm(16U, 17U).ok());
  boompi::audio::Mono16kFrame output{};
  auto result = bridge.TryPop(&output);
  BOOMPI_EXPECT(
      context,
      result.code == boompi::audio::AudioDspFrameBridgeCode::kNeedMoreInput);
  BOOMPI_EXPECT(context, !output.HasValidLength());

  const auto first = MakeInput(16U, 17U, 50U);
  BOOMPI_EXPECT(context, bridge.Process(first).needs_more_input());
  BOOMPI_EXPECT(context,
                bridge.Process(MakeInput(16U, 17U, 51U))
                    .output_available());
  BOOMPI_EXPECT(context, bridge.TryPop(&output).output_available());
  ExpectValidOutput(context, output, first);
}

void TestDisarmAndOldGenerationIsolation(
    boompi::test::TestContext& context) {
  CopyMicLeftBlockProcessor processor;
  boompi::audio::AudioDspFrameBridge16k bridge;
  BOOMPI_EXPECT(
      context,
      boompi::audio::AudioDspFrameBridge16k::Create(&processor, &bridge).ok());
  BOOMPI_EXPECT(context, bridge.Arm(10U, 11U).ok());
  BOOMPI_EXPECT(context,
                bridge.Process(MakeInput(10U, 11U, 0U)).needs_more_input());
  const std::size_t calls_before_disarm = processor.call_count();
  bridge.Disarm();

  boompi::audio::Mono16kFrame output{};
  auto result = bridge.TryPop(&output);
  BOOMPI_EXPECT(
      context,
      result.code == boompi::audio::AudioDspFrameBridgeCode::kNotArmed);
  BOOMPI_EXPECT(context, !output.HasValidLength());
  BOOMPI_EXPECT(context, bridge.Arm(11U, 12U).ok());

  result = bridge.Process(MakeInput(10U, 11U, 1U));
  BOOMPI_EXPECT(
      context,
      result.code == boompi::audio::AudioDspFrameBridgeCode::kEpochMismatch);
  BOOMPI_EXPECT(context, processor.call_count() == calls_before_disarm);
  result = bridge.Process(MakeInput(11U, 99U, 20U));
  BOOMPI_EXPECT(
      context,
      result.code == boompi::audio::AudioDspFrameBridgeCode::kStreamMismatch);
  BOOMPI_EXPECT(context, processor.call_count() == calls_before_disarm);

  const auto first = MakeInput(11U, 12U, 20U);
  const auto second = MakeInput(11U, 12U, 21U);
  BOOMPI_EXPECT(context, bridge.Process(first).needs_more_input());
  result = bridge.Process(MakeInput(10U, 11U, 2U));
  BOOMPI_EXPECT(
      context,
      result.code == boompi::audio::AudioDspFrameBridgeCode::kEpochMismatch);
  BOOMPI_EXPECT(context, bridge.Process(second).output_available());
  BOOMPI_EXPECT(context, bridge.TryPop(&output).output_available());
  ExpectValidOutput(context, output, first);
}

void TestDiscontinuityFlushesQueuedData(
    boompi::test::TestContext& context) {
  CopyMicLeftBlockProcessor processor;
  boompi::audio::AudioDspFrameBridge16k bridge;
  BOOMPI_EXPECT(
      context,
      boompi::audio::AudioDspFrameBridge16k::Create(&processor, &bridge).ok());
  BOOMPI_EXPECT(context, bridge.Arm(20U, 21U).ok());
  for (std::uint32_t sequence = 0U; sequence < 4U; ++sequence) {
    bridge.Process(MakeInput(20U, 21U, sequence));
  }

  auto result = bridge.Process(MakeInput(20U, 21U, 4U, true));
  BOOMPI_EXPECT(
      context,
      result.code == boompi::audio::AudioDspFrameBridgeCode::kDiscontinuity);
  BOOMPI_EXPECT(context, result.requires_new_generation());
  BOOMPI_EXPECT(context, result.available_outputs == 0U);
  boompi::audio::Mono16kFrame output{};
  result = bridge.TryPop(&output);
  BOOMPI_EXPECT(
      context,
      result.code == boompi::audio::AudioDspFrameBridgeCode::kFaulted);
  BOOMPI_EXPECT(context, !output.HasValidLength());
  result = bridge.Process(MakeInput(20U, 21U, 5U));
  BOOMPI_EXPECT(
      context,
      result.code == boompi::audio::AudioDspFrameBridgeCode::kFaulted);

  BOOMPI_EXPECT(context, bridge.Arm(21U, 22U).ok());
  const auto first = MakeInput(21U, 22U, 100U);
  BOOMPI_EXPECT(context, bridge.Process(first).needs_more_input());
  BOOMPI_EXPECT(context,
                bridge.Process(MakeInput(21U, 22U, 101U))
                    .output_available());
  BOOMPI_EXPECT(context, bridge.TryPop(&output).output_available());
  ExpectValidOutput(context, output, first);
}

void TestBackendFailureAndNonExactOutput(
    boompi::test::TestContext& context) {
  CopyMicLeftBlockProcessor failing_processor;
  failing_processor.fail_on_call(2U);
  boompi::audio::AudioDspFrameBridge16k bridge;
  BOOMPI_EXPECT(
      context,
      boompi::audio::AudioDspFrameBridge16k::Create(&failing_processor,
                                                     &bridge)
          .ok());
  BOOMPI_EXPECT(context, bridge.Arm(30U, 31U).ok());
  BOOMPI_EXPECT(context,
                bridge.Process(MakeInput(30U, 31U, 0U)).needs_more_input());
  auto result = bridge.Process(MakeInput(30U, 31U, 1U));
  BOOMPI_EXPECT(
      context,
      result.code == boompi::audio::AudioDspFrameBridgeCode::kBackendFailure);
  BOOMPI_EXPECT(context, result.requires_new_generation());
  BOOMPI_EXPECT(context, result.available_outputs == 0U);
  BOOMPI_EXPECT(
      context,
      bridge.Process(MakeInput(30U, 31U, 2U)).code ==
          boompi::audio::AudioDspFrameBridgeCode::kFaulted);

  BOOMPI_EXPECT(context, bridge.Arm(31U, 32U).ok());
  const auto recovered_first = MakeInput(31U, 32U, 10U);
  BOOMPI_EXPECT(context, bridge.Process(recovered_first).needs_more_input());
  BOOMPI_EXPECT(context,
                bridge.Process(MakeInput(31U, 32U, 11U))
                    .output_available());
  boompi::audio::Mono16kFrame recovered_output{};
  BOOMPI_EXPECT(context,
                bridge.TryPop(&recovered_output).output_available());
  ExpectValidOutput(context, recovered_output, recovered_first);

  CopyMicLeftBlockProcessor short_processor;
  short_processor.report_samples(255U);
  boompi::audio::AudioDspFrameBridge16k short_bridge;
  BOOMPI_EXPECT(
      context,
      boompi::audio::AudioDspFrameBridge16k::Create(&short_processor,
                                                     &short_bridge)
          .ok());
  BOOMPI_EXPECT(context, short_bridge.Arm(40U, 41U).ok());
  result = short_bridge.Process(MakeInput(40U, 41U, 0U));
  BOOMPI_EXPECT(
      context,
      result.code == boompi::audio::AudioDspFrameBridgeCode::kBackendFailure);
  BOOMPI_EXPECT(context, result.processed_blocks == 0U);

  CopyMicLeftBlockProcessor long_processor;
  long_processor.report_samples(257U);
  boompi::audio::AudioDspFrameBridge16k long_bridge;
  BOOMPI_EXPECT(
      context,
      boompi::audio::AudioDspFrameBridge16k::Create(&long_processor,
                                                     &long_bridge)
          .ok());
  BOOMPI_EXPECT(context, long_bridge.Arm(50U, 51U).ok());
  result = long_bridge.Process(MakeInput(50U, 51U, 0U));
  BOOMPI_EXPECT(
      context,
      result.code == boompi::audio::AudioDspFrameBridgeCode::kBackendFailure);
}

void TestBoundedOutputOverflow(boompi::test::TestContext& context) {
  CopyMicLeftBlockProcessor processor;
  boompi::audio::AudioDspFrameBridge16k bridge;
  BOOMPI_EXPECT(
      context,
      boompi::audio::AudioDspFrameBridge16k::Create(&processor, &bridge).ok());
  BOOMPI_EXPECT(context, bridge.Arm(60U, 61U).ok());

  for (std::uint32_t sequence = 0U; sequence < 4U; ++sequence) {
    bridge.Process(MakeInput(60U, 61U, sequence));
  }
  auto result = bridge.Process(MakeInput(60U, 61U, 4U));
  BOOMPI_EXPECT(context, result.output_available());
  BOOMPI_EXPECT(context,
                result.available_outputs ==
                    boompi::audio::kDspFrameBridgeOutputCapacity);
  const std::size_t calls_before_overflow = processor.call_count();
  result = bridge.Process(MakeInput(60U, 61U, 5U));
  BOOMPI_EXPECT(
      context,
      result.code == boompi::audio::AudioDspFrameBridgeCode::kOverflow);
  BOOMPI_EXPECT(context, result.requires_new_generation());
  BOOMPI_EXPECT(context, result.available_outputs == 0U);
  BOOMPI_EXPECT(context, processor.call_count() == calls_before_overflow);

  boompi::audio::Mono16kFrame output{};
  BOOMPI_EXPECT(
      context,
      bridge.TryPop(&output).code ==
          boompi::audio::AudioDspFrameBridgeCode::kFaulted);
  BOOMPI_EXPECT(context, !output.HasValidLength());

  BOOMPI_EXPECT(context, bridge.Arm(61U, 62U).ok());
  const auto recovered_first = MakeInput(61U, 62U, 20U);
  BOOMPI_EXPECT(context, bridge.Process(recovered_first).needs_more_input());
  BOOMPI_EXPECT(context,
                bridge.Process(MakeInput(61U, 62U, 21U))
                    .output_available());
  BOOMPI_EXPECT(context, bridge.TryPop(&output).output_available());
  ExpectValidOutput(context, output, recovered_first);
}

void TestLongSequenceSampleConservation(
    boompi::test::TestContext& context) {
  CopyMicLeftBlockProcessor processor;
  boompi::audio::AudioDspFrameBridge16k bridge;
  BOOMPI_EXPECT(
      context,
      boompi::audio::AudioDspFrameBridge16k::Create(&processor, &bridge).ok());
  BOOMPI_EXPECT(context, bridge.Arm(70U, 71U).ok());

  constexpr std::uint32_t kFrameCount = 1000U;
  std::uint32_t output_sequence = 0U;
  for (std::uint32_t input_sequence = 0U; input_sequence < kFrameCount;
       ++input_sequence) {
    auto result = bridge.Process(MakeInput(70U, 71U, input_sequence));
    BOOMPI_EXPECT(
        context,
        result.code == boompi::audio::AudioDspFrameBridgeCode::kNeedMoreInput ||
            result.code ==
                boompi::audio::AudioDspFrameBridgeCode::kOutputAvailable);
    while (result.available_outputs != 0U) {
      boompi::audio::Mono16kFrame output{};
      result = bridge.TryPop(&output);
      BOOMPI_EXPECT(context, result.output_available());
      ExpectValidOutput(context, output,
                        MakeInput(70U, 71U, output_sequence));
      ++output_sequence;
    }
  }

  BOOMPI_EXPECT(context, output_sequence == kFrameCount);
  BOOMPI_EXPECT(context, processor.call_count() == 1250U);
  boompi::audio::Mono16kFrame output{};
  BOOMPI_EXPECT(
      context,
      bridge.TryPop(&output).code ==
          boompi::audio::AudioDspFrameBridgeCode::kNeedMoreInput);
  BOOMPI_EXPECT(context, !output.HasValidLength());
}

}  // namespace

int main() {
  boompi::test::TestContext context;
  TestCreateAndArmValidation(context);
  TestExactFourFrameCadenceAndMetadata(context);
  TestArmFlushesResidualAndQueuedOutput(context);
  TestDisarmAndOldGenerationIsolation(context);
  TestDiscontinuityFlushesQueuedData(context);
  TestBackendFailureAndNonExactOutput(context);
  TestBoundedOutputOverflow(context);
  TestLongSequenceSampleConservation(context);

  if (context.failures() != 0U) {
    std::cerr << context.failures()
              << " DSP frame bridge expectation(s) failed\n";
    return 1;
  }
  std::cout << "boomPI DSP frame bridge tests passed\n";
  return 0;
}
