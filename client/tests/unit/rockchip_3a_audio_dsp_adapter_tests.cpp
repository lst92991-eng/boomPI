#include "boompi/platform/rv1106/rockchip_3a_audio_dsp_adapter.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>

#include "boompi/test/test_context.h"

#define BOOMPI_EXPECT(context, expression) \
  (context).Expect((expression), #expression, __FILE__, __LINE__)

namespace {

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
  frame.metadata.turn_id = sequence + 200U;
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

boompi::audio::AudioDspEngineConfig ValidConfig() {
  return {boompi::audio::AudioDspReferenceSource::kHardwareCapture,
          boompi::audio::AudioDspReferenceLayout::kStereo,
          boompi::audio::kBoompiV1AudioDspFeatures};
}

bool MetadataEqual(const boompi::audio::AudioFrameMetadata& left,
                   const boompi::audio::AudioFrameMetadata& right) {
  return left.monotonic_timestamp_us == right.monotonic_timestamp_us &&
         left.sequence == right.sequence &&
         left.stream_id == right.stream_id &&
         left.turn_id == right.turn_id && left.epoch == right.epoch &&
         left.discontinuity == right.discontinuity;
}

class FakeRockchip3aSession final
    : public boompi::platform::rv1106::Rockchip3aVendorSession {
 public:
  boompi::platform::rv1106::Rockchip3aVendorInitResult
  Initialize() noexcept override {
    ++initialize_calls_;
    if (fail_next_initialize_) {
      fail_next_initialize_ = false;
      initialized_ = false;
      return {boompi::platform::rv1106::
                  Rockchip3aVendorInitCode::kBackendInitializationFailed};
    }
    if (initialized_) {
      return {boompi::platform::rv1106::
                  Rockchip3aVendorInitCode::kAlreadyInitialized};
    }
    initialized_ = true;
    block_stream_sample_ = next_generation_start_sample_;
    return {
        boompi::platform::rv1106::Rockchip3aVendorInitCode::kReady};
  }

  boompi::platform::rv1106::Rockchip3aVendorProcessResult Process(
      const boompi::platform::rv1106::Rockchip3aInterleavedBlock16k& input,
      boompi::audio::DspMonoBlock16k16ms* const output) noexcept override {
    ++process_calls_;
    if (!initialized_) {
      return {boompi::platform::rv1106::
                  Rockchip3aVendorProcessCode::kNotInitialized,
              0U};
    }
    if (fail_on_process_call_ != 0U &&
        process_calls_ == fail_on_process_call_) {
      return {boompi::platform::rv1106::
                  Rockchip3aVendorProcessCode::kBackendRejectedInput,
              0U};
    }

    for (std::size_t sample = 0U;
         sample < boompi::audio::kDspBlockSamplesPerChannel16k; ++sample) {
      const std::uint64_t stream_sample = block_stream_sample_ + sample;
      const auto source_sequence = static_cast<std::uint32_t>(
          stream_sample / boompi::audio::kSamplesPer16k20ms);
      const std::size_t source_sample =
          static_cast<std::size_t>(
              stream_sample % boompi::audio::kSamplesPer16k20ms);
      for (std::size_t channel = 0U;
           channel < boompi::platform::rv1106::kRockchip3aInputChannels;
           ++channel) {
        const std::size_t interleaved_index =
            sample * boompi::platform::rv1106::kRockchip3aInputChannels +
            channel;
        if (input[interleaved_index] !=
            SampleFor(source_sequence, source_sample, channel)) {
          interleaving_valid_ = false;
        }
      }
      if (output != nullptr) {
        (*output)[sample] = input[
            sample * boompi::platform::rv1106::kRockchip3aInputChannels];
      }
    }
    block_stream_sample_ +=
        boompi::audio::kDspBlockSamplesPerChannel16k;

    const std::uint32_t produced_bytes =
        short_on_process_call_ != 0U &&
                process_calls_ == short_on_process_call_
            ? boompi::platform::rv1106::kRockchip3aOutputBytes - 2U
            : boompi::platform::rv1106::kRockchip3aOutputBytes;
    return {boompi::platform::rv1106::
                Rockchip3aVendorProcessCode::kProduced,
            produced_bytes};
  }

  void Destroy() noexcept override {
    ++destroy_calls_;
    initialized_ = false;
  }

  void fail_next_initialize() noexcept { fail_next_initialize_ = true; }
  void fail_on_process_call(const std::size_t call) noexcept {
    fail_on_process_call_ = call;
  }
  void short_on_process_call(const std::size_t call) noexcept {
    short_on_process_call_ = call;
  }
  void expect_first_sequence(const std::uint32_t sequence) noexcept {
    next_generation_start_sample_ =
        static_cast<std::uint64_t>(sequence) *
        boompi::audio::kSamplesPer16k20ms;
  }
  std::size_t initialize_calls() const noexcept { return initialize_calls_; }
  std::size_t process_calls() const noexcept { return process_calls_; }
  std::size_t destroy_calls() const noexcept { return destroy_calls_; }
  bool interleaving_valid() const noexcept { return interleaving_valid_; }

 private:
  std::size_t initialize_calls_{0U};
  std::size_t process_calls_{0U};
  std::size_t destroy_calls_{0U};
  std::size_t fail_on_process_call_{0U};
  std::size_t short_on_process_call_{0U};
  std::uint64_t block_stream_sample_{0U};
  std::uint64_t next_generation_start_sample_{0U};
  bool fail_next_initialize_{false};
  bool initialized_{false};
  bool interleaving_valid_{true};
};

void ExpectOutputMatches(boompi::test::TestContext& context,
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

void TestConfigurationAndArmValidation(
    boompi::test::TestContext& context) {
  FakeRockchip3aSession session;
  boompi::platform::rv1106::Rockchip3aAudioDspAdapter adapter;
  BOOMPI_EXPECT(
      context,
      !boompi::platform::rv1106::Rockchip3aAudioDspAdapter::Create(
           nullptr, ValidConfig(), &adapter)
           .ok());
  BOOMPI_EXPECT(
      context,
      !boompi::platform::rv1106::Rockchip3aAudioDspAdapter::Create(
           &session, ValidConfig(), nullptr)
           .ok());

  auto config = ValidConfig();
  config.reference_layout = boompi::audio::AudioDspReferenceLayout::kMonoLeft;
  BOOMPI_EXPECT(
      context,
      !boompi::platform::rv1106::Rockchip3aAudioDspAdapter::Create(
           &session, config, &adapter)
           .ok());
  config = ValidConfig();
  config.requested_features =
      boompi::audio::ToMask(
          boompi::audio::AudioDspFeature::kEchoCancellation);
  BOOMPI_EXPECT(
      context,
      !boompi::platform::rv1106::Rockchip3aAudioDspAdapter::Create(
           &session, config, &adapter)
           .ok());

  BOOMPI_EXPECT(
      context,
      boompi::platform::rv1106::Rockchip3aAudioDspAdapter::Create(
          &session, ValidConfig(), &adapter)
          .ok());
  BOOMPI_EXPECT(
      context,
      !boompi::platform::rv1106::Rockchip3aAudioDspAdapter::Create(
           &session, ValidConfig(), &adapter)
           .ok());
  BOOMPI_EXPECT(context, !adapter.Arm(0U, 1U).ok());
  BOOMPI_EXPECT(context, !adapter.Arm(1U, 0U).ok());
  BOOMPI_EXPECT(context, adapter.Arm(1U, 2U).ok());
  BOOMPI_EXPECT(context, session.initialize_calls() == 1U);
  BOOMPI_EXPECT(context, !adapter.Arm(1U, 3U).ok());
  BOOMPI_EXPECT(context, session.initialize_calls() == 1U);
  BOOMPI_EXPECT(context, session.destroy_calls() == 0U);
}

void TestCadencePackingMetadataAndConservation(
    boompi::test::TestContext& context) {
  FakeRockchip3aSession session;
  boompi::platform::rv1106::Rockchip3aAudioDspAdapter adapter;
  BOOMPI_EXPECT(
      context,
      boompi::platform::rv1106::Rockchip3aAudioDspAdapter::Create(
          &session, ValidConfig(), &adapter)
          .ok());
  BOOMPI_EXPECT(context, adapter.Arm(10U, 11U).ok());

  constexpr std::array<std::uint32_t, 4U> kExpectedBlocks{{1U, 1U, 1U, 2U}};
  constexpr std::array<std::uint32_t, 4U> kExpectedOutputs{{0U, 1U, 1U, 2U}};
  std::uint32_t next_output_sequence = 0U;
  for (std::uint32_t sequence = 0U; sequence < 4U; ++sequence) {
    auto result = adapter.Process(MakeInput(10U, 11U, sequence));
    BOOMPI_EXPECT(context,
                  result.processed_blocks == kExpectedBlocks[sequence]);
    BOOMPI_EXPECT(context,
                  result.available_outputs == kExpectedOutputs[sequence]);
    while (result.available_outputs != 0U) {
      boompi::audio::Mono16kFrame output{};
      result = adapter.TryPop(&output);
      BOOMPI_EXPECT(context, result.output_available());
      ExpectOutputMatches(context, output,
                          MakeInput(10U, 11U, next_output_sequence));
      ++next_output_sequence;
    }
  }

  BOOMPI_EXPECT(context, session.process_calls() == 5U);
  BOOMPI_EXPECT(context, session.interleaving_valid());
  BOOMPI_EXPECT(context, next_output_sequence == 4U);
}

void TestInitializationFailureConsumesGeneration(
    boompi::test::TestContext& context) {
  FakeRockchip3aSession session;
  session.fail_next_initialize();
  boompi::platform::rv1106::Rockchip3aAudioDspAdapter adapter;
  BOOMPI_EXPECT(
      context,
      boompi::platform::rv1106::Rockchip3aAudioDspAdapter::Create(
          &session, ValidConfig(), &adapter)
          .ok());
  BOOMPI_EXPECT(context, !adapter.Arm(20U, 21U).ok());
  BOOMPI_EXPECT(context, session.initialize_calls() == 1U);
  BOOMPI_EXPECT(context, session.destroy_calls() == 1U);
  BOOMPI_EXPECT(context, !adapter.Arm(20U, 22U).ok());
  BOOMPI_EXPECT(context, session.initialize_calls() == 1U);
  session.expect_first_sequence(0U);
  BOOMPI_EXPECT(context, adapter.Arm(21U, 22U).ok());
  BOOMPI_EXPECT(context, session.initialize_calls() == 2U);
  BOOMPI_EXPECT(
      context,
      adapter.Process(MakeInput(21U, 22U, 0U)).needs_more_input());
}

void TestBackendFailureAndReinitialize(
    boompi::test::TestContext& context) {
  FakeRockchip3aSession session;
  session.fail_on_process_call(2U);
  boompi::platform::rv1106::Rockchip3aAudioDspAdapter adapter;
  BOOMPI_EXPECT(
      context,
      boompi::platform::rv1106::Rockchip3aAudioDspAdapter::Create(
          &session, ValidConfig(), &adapter)
          .ok());
  BOOMPI_EXPECT(context, adapter.Arm(30U, 31U).ok());
  BOOMPI_EXPECT(
      context,
      adapter.Process(MakeInput(30U, 31U, 0U)).needs_more_input());
  auto result = adapter.Process(MakeInput(30U, 31U, 1U));
  BOOMPI_EXPECT(
      context,
      result.code == boompi::audio::AudioDspFrameBridgeCode::kBackendFailure);
  BOOMPI_EXPECT(context, result.requires_new_generation());
  BOOMPI_EXPECT(context, session.destroy_calls() == 1U);
  BOOMPI_EXPECT(
      context,
      adapter.Process(MakeInput(30U, 31U, 2U)).code ==
          boompi::audio::AudioDspFrameBridgeCode::kFaulted);

  session.expect_first_sequence(10U);
  BOOMPI_EXPECT(context, adapter.Arm(31U, 32U).ok());
  BOOMPI_EXPECT(context, session.initialize_calls() == 2U);
  const auto first = MakeInput(31U, 32U, 10U);
  BOOMPI_EXPECT(context, adapter.Process(first).needs_more_input());
  BOOMPI_EXPECT(
      context,
      adapter.Process(MakeInput(31U, 32U, 11U)).output_available());
  boompi::audio::Mono16kFrame output{};
  BOOMPI_EXPECT(context, adapter.TryPop(&output).output_available());
  ExpectOutputMatches(context, output, first);
  BOOMPI_EXPECT(context, session.interleaving_valid());
}

void TestActiveRearmFlushesAndReinitializes(
    boompi::test::TestContext& context) {
  FakeRockchip3aSession session;
  boompi::platform::rv1106::Rockchip3aAudioDspAdapter adapter;
  BOOMPI_EXPECT(
      context,
      boompi::platform::rv1106::Rockchip3aAudioDspAdapter::Create(
          &session, ValidConfig(), &adapter)
          .ok());
  BOOMPI_EXPECT(context, adapter.Arm(35U, 36U).ok());
  for (std::uint32_t sequence = 0U; sequence < 4U; ++sequence) {
    static_cast<void>(adapter.Process(MakeInput(35U, 36U, sequence)));
  }

  session.expect_first_sequence(50U);
  BOOMPI_EXPECT(context, adapter.Arm(36U, 37U).ok());
  BOOMPI_EXPECT(context, session.initialize_calls() == 2U);
  BOOMPI_EXPECT(context, session.destroy_calls() == 1U);
  boompi::audio::Mono16kFrame output{};
  BOOMPI_EXPECT(
      context,
      adapter.TryPop(&output).code ==
          boompi::audio::AudioDspFrameBridgeCode::kNeedMoreInput);
  BOOMPI_EXPECT(context, !output.HasValidLength());

  const auto first = MakeInput(36U, 37U, 50U);
  BOOMPI_EXPECT(context, adapter.Process(first).needs_more_input());
  BOOMPI_EXPECT(
      context,
      adapter.Process(MakeInput(36U, 37U, 51U)).output_available());
  BOOMPI_EXPECT(context, adapter.TryPop(&output).output_available());
  ExpectOutputMatches(context, output, first);
  BOOMPI_EXPECT(context, session.interleaving_valid());

  adapter.Disarm();
  adapter.Disarm();
  BOOMPI_EXPECT(context, session.destroy_calls() == 2U);
}

void TestLongSequenceSampleConservation(
    boompi::test::TestContext& context) {
  FakeRockchip3aSession session;
  boompi::platform::rv1106::Rockchip3aAudioDspAdapter adapter;
  BOOMPI_EXPECT(
      context,
      boompi::platform::rv1106::Rockchip3aAudioDspAdapter::Create(
          &session, ValidConfig(), &adapter)
          .ok());
  BOOMPI_EXPECT(context, adapter.Arm(38U, 39U).ok());

  constexpr std::uint32_t kFrameCount = 1000U;
  std::uint32_t output_sequence = 0U;
  for (std::uint32_t input_sequence = 0U; input_sequence < kFrameCount;
       ++input_sequence) {
    auto result = adapter.Process(MakeInput(38U, 39U, input_sequence));
    BOOMPI_EXPECT(
        context,
        result.code ==
                boompi::audio::AudioDspFrameBridgeCode::kNeedMoreInput ||
            result.code ==
                boompi::audio::AudioDspFrameBridgeCode::kOutputAvailable);
    while (result.available_outputs != 0U) {
      boompi::audio::Mono16kFrame output{};
      result = adapter.TryPop(&output);
      BOOMPI_EXPECT(context, result.output_available());
      ExpectOutputMatches(context, output,
                          MakeInput(38U, 39U, output_sequence));
      ++output_sequence;
    }
  }

  BOOMPI_EXPECT(context, output_sequence == kFrameCount);
  BOOMPI_EXPECT(context, session.process_calls() == 1250U);
  BOOMPI_EXPECT(context, session.interleaving_valid());
}

void TestShortReturnAndGenerationFaults(
    boompi::test::TestContext& context) {
  FakeRockchip3aSession short_session;
  short_session.short_on_process_call(1U);
  boompi::platform::rv1106::Rockchip3aAudioDspAdapter short_adapter;
  BOOMPI_EXPECT(
      context,
      boompi::platform::rv1106::Rockchip3aAudioDspAdapter::Create(
          &short_session, ValidConfig(), &short_adapter)
          .ok());
  BOOMPI_EXPECT(context, short_adapter.Arm(40U, 41U).ok());
  auto result = short_adapter.Process(MakeInput(40U, 41U, 0U));
  BOOMPI_EXPECT(
      context,
      result.code == boompi::audio::AudioDspFrameBridgeCode::kBackendFailure);
  BOOMPI_EXPECT(context, short_session.destroy_calls() == 1U);

  FakeRockchip3aSession session;
  boompi::platform::rv1106::Rockchip3aAudioDspAdapter adapter;
  BOOMPI_EXPECT(
      context,
      boompi::platform::rv1106::Rockchip3aAudioDspAdapter::Create(
          &session, ValidConfig(), &adapter)
          .ok());
  BOOMPI_EXPECT(context, adapter.Arm(50U, 51U).ok());
  result = adapter.Process(MakeInput(49U, 51U, 0U));
  BOOMPI_EXPECT(
      context,
      result.code == boompi::audio::AudioDspFrameBridgeCode::kEpochMismatch);
  BOOMPI_EXPECT(context, session.process_calls() == 0U);
  BOOMPI_EXPECT(context, session.destroy_calls() == 0U);

  BOOMPI_EXPECT(
      context,
      adapter.Process(MakeInput(50U, 51U, 0U)).needs_more_input());
  result = adapter.Process(MakeInput(50U, 51U, 1U, true));
  BOOMPI_EXPECT(
      context,
      result.code == boompi::audio::AudioDspFrameBridgeCode::kDiscontinuity);
  BOOMPI_EXPECT(context, session.destroy_calls() == 1U);
  boompi::audio::Mono16kFrame output{};
  BOOMPI_EXPECT(
      context,
      adapter.TryPop(&output).code ==
          boompi::audio::AudioDspFrameBridgeCode::kFaulted);
  BOOMPI_EXPECT(context, !output.HasValidLength());
}

}  // namespace

int main() {
  boompi::test::TestContext context;
  TestConfigurationAndArmValidation(context);
  TestCadencePackingMetadataAndConservation(context);
  TestInitializationFailureConsumesGeneration(context);
  TestBackendFailureAndReinitialize(context);
  TestActiveRearmFlushesAndReinitializes(context);
  TestLongSequenceSampleConservation(context);
  TestShortReturnAndGenerationFaults(context);

  if (context.failures() != 0U) {
    std::cerr << context.failures()
              << " Rockchip 3A adapter expectation(s) failed\n";
    return 1;
  }
  std::cout << "boomPI Rockchip 3A adapter tests passed\n";
  return 0;
}
