#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>

#include "boompi/audio/capture_channel_mapper.h"
#include "boompi/test/test_context.h"

#define BOOMPI_EXPECT(context, expression) \
  (context).Expect((expression), #expression, __FILE__, __LINE__)

namespace {

constexpr std::size_t RoleIndex(const boompi::audio::CaptureRole role) {
  return static_cast<std::size_t>(role);
}

boompi::audio::ChannelMap IdentityMap() {
  boompi::audio::ChannelMap channel_map{};
  channel_map[boompi::audio::CaptureRole::kMicLeft] = {0U, 1};
  channel_map[boompi::audio::CaptureRole::kMicRight] = {1U, 1};
  channel_map[boompi::audio::CaptureRole::kReferenceLeft] = {2U, 1};
  channel_map[boompi::audio::CaptureRole::kReferenceRight] = {3U, 1};
  return channel_map;
}

boompi::audio::CaptureFrame MakeFourChannelFrame() {
  boompi::audio::CaptureFrame frame{};
  frame.format = {48000U, 20U, 4U,
                  boompi::audio::SampleFormat::kPcmS16Le};
  frame.samples_per_channel = 960U;
  return frame;
}

void FillDistinctChannels(boompi::audio::CaptureFrame* const frame) {
  for (std::size_t sample_index = 0U;
       sample_index < boompi::audio::kSamplesPer48k20ms; ++sample_index) {
    for (std::size_t input_slot = 0U;
         input_slot < boompi::audio::kDspCaptureChannelCount; ++input_slot) {
      const auto value = static_cast<std::int16_t>(
          sample_index * boompi::audio::kDspCaptureChannelCount + input_slot);
      frame->samples[sample_index * boompi::audio::kDspCaptureChannelCount +
                     input_slot] = value;
    }
  }
}

boompi::audio::CaptureChannelMapper MakeMapper(
    boompi::test::TestContext& context,
    const boompi::audio::ChannelMap& channel_map) {
  boompi::audio::CaptureChannelMapper mapper;
  BOOMPI_EXPECT(
      context,
      boompi::audio::CaptureChannelMapper::Create(channel_map, &mapper).ok());
  return mapper;
}

void TestIdentityAndFullFrameBoundary(boompi::test::TestContext& context) {
  auto mapper = MakeMapper(context, IdentityMap());
  auto frame = MakeFourChannelFrame();
  FillDistinctChannels(&frame);
  boompi::audio::CapturePlanes48k output{};

  const auto result = mapper.Process(frame, &output);
  BOOMPI_EXPECT(context, result.ok());
  BOOMPI_EXPECT(context, result.saturated_samples == 0U);
  for (std::size_t sample_index = 0U;
       sample_index < boompi::audio::kSamplesPer48k20ms; ++sample_index) {
    for (std::size_t role_index = 0U;
         role_index < boompi::audio::kDspCaptureChannelCount; ++role_index) {
      const auto expected = static_cast<std::int16_t>(
          sample_index * boompi::audio::kDspCaptureChannelCount + role_index);
      BOOMPI_EXPECT(context, output[role_index][sample_index] == expected);
    }
  }

  constexpr std::size_t kLastSample =
      boompi::audio::kSamplesPer48k20ms - 1U;
  constexpr std::size_t kLastInterleaved =
      boompi::audio::kCaptureFrameInterleavedSampleCapacity - 1U;
  BOOMPI_EXPECT(context,
                frame.samples[kLastInterleaved] == output[3U][kLastSample]);
}

void TestPermutation(boompi::test::TestContext& context) {
  auto channel_map = IdentityMap();
  channel_map[boompi::audio::CaptureRole::kMicLeft].input_slot = 2U;
  channel_map[boompi::audio::CaptureRole::kMicRight].input_slot = 0U;
  channel_map[boompi::audio::CaptureRole::kReferenceLeft].input_slot = 3U;
  channel_map[boompi::audio::CaptureRole::kReferenceRight].input_slot = 1U;
  auto mapper = MakeMapper(context, channel_map);
  auto frame = MakeFourChannelFrame();
  FillDistinctChannels(&frame);
  boompi::audio::CapturePlanes48k output{};

  const auto result = mapper.Process(frame, &output);
  BOOMPI_EXPECT(context, result.ok());
  const std::size_t sample_index = 731U;
  BOOMPI_EXPECT(
      context,
      output[RoleIndex(boompi::audio::CaptureRole::kMicLeft)][sample_index] ==
          frame.samples[sample_index * 4U + 2U]);
  BOOMPI_EXPECT(
      context,
      output[RoleIndex(boompi::audio::CaptureRole::kMicRight)][sample_index] ==
          frame.samples[sample_index * 4U]);
  BOOMPI_EXPECT(
      context,
      output[RoleIndex(boompi::audio::CaptureRole::kReferenceLeft)]
            [sample_index] == frame.samples[sample_index * 4U + 3U]);
  BOOMPI_EXPECT(
      context,
      output[RoleIndex(boompi::audio::CaptureRole::kReferenceRight)]
            [sample_index] == frame.samples[sample_index * 4U + 1U]);
}

void TestPolarityAndSaturation(boompi::test::TestContext& context) {
  auto channel_map = IdentityMap();
  channel_map[boompi::audio::CaptureRole::kMicLeft].polarity = -1;
  channel_map[boompi::audio::CaptureRole::kReferenceRight].polarity = -1;
  auto mapper = MakeMapper(context, channel_map);
  auto frame = MakeFourChannelFrame();

  for (std::size_t sample_index = 0U;
       sample_index < boompi::audio::kSamplesPer48k20ms; ++sample_index) {
    frame.samples[sample_index * 4U] = -123;
    frame.samples[sample_index * 4U + 1U] = 234;
    frame.samples[sample_index * 4U + 2U] = -345;
    frame.samples[sample_index * 4U + 3U] = 456;
  }
  frame.samples[0U] = std::numeric_limits<std::int16_t>::min();
  frame.samples[(boompi::audio::kSamplesPer48k20ms - 1U) * 4U + 3U] =
      std::numeric_limits<std::int16_t>::min();

  boompi::audio::CapturePlanes48k output{};
  const auto result = mapper.Process(frame, &output);
  BOOMPI_EXPECT(context, result.ok());
  BOOMPI_EXPECT(context, result.saturated_samples == 2U);
  BOOMPI_EXPECT(
      context,
      output[RoleIndex(boompi::audio::CaptureRole::kMicLeft)][0U] ==
          std::numeric_limits<std::int16_t>::max());
  BOOMPI_EXPECT(
      context,
      output[RoleIndex(boompi::audio::CaptureRole::kMicLeft)][1U] == 123);
  BOOMPI_EXPECT(
      context,
      output[RoleIndex(boompi::audio::CaptureRole::kMicRight)][1U] == 234);
  BOOMPI_EXPECT(
      context,
      output[RoleIndex(boompi::audio::CaptureRole::kReferenceLeft)][1U] ==
          -345);
  BOOMPI_EXPECT(
      context,
      output[RoleIndex(boompi::audio::CaptureRole::kReferenceRight)][1U] ==
          -456);
  BOOMPI_EXPECT(
      context,
      output[RoleIndex(boompi::audio::CaptureRole::kReferenceRight)]
            [boompi::audio::kSamplesPer48k20ms - 1U] ==
          std::numeric_limits<std::int16_t>::max());
}

void TestInvalidMaps(boompi::test::TestContext& context) {
  auto channel_map = IdentityMap();
  BOOMPI_EXPECT(
      context,
      !boompi::audio::CaptureChannelMapper::Create(channel_map, nullptr).ok());

  boompi::audio::CaptureChannelMapper mapper;
  channel_map[boompi::audio::CaptureRole::kMicLeft].input_slot = 4U;
  BOOMPI_EXPECT(
      context,
      !boompi::audio::CaptureChannelMapper::Create(channel_map, &mapper).ok());

  channel_map = IdentityMap();
  channel_map[boompi::audio::CaptureRole::kMicRight].input_slot = 0U;
  BOOMPI_EXPECT(
      context,
      !boompi::audio::CaptureChannelMapper::Create(channel_map, &mapper).ok());

  for (const std::int8_t invalid_polarity :
       {static_cast<std::int8_t>(0), static_cast<std::int8_t>(2),
        static_cast<std::int8_t>(-2)}) {
    channel_map = IdentityMap();
    channel_map[boompi::audio::CaptureRole::kReferenceLeft].polarity =
        invalid_polarity;
    BOOMPI_EXPECT(
        context,
        !boompi::audio::CaptureChannelMapper::Create(channel_map, &mapper)
             .ok());
  }
}

void TestFailedUpdatePreservesMapper(boompi::test::TestContext& context) {
  auto mapper = MakeMapper(context, IdentityMap());
  auto invalid_map = IdentityMap();
  invalid_map[boompi::audio::CaptureRole::kMicRight].input_slot = 0U;
  BOOMPI_EXPECT(
      context,
      !boompi::audio::CaptureChannelMapper::Create(invalid_map, &mapper).ok());

  auto frame = MakeFourChannelFrame();
  FillDistinctChannels(&frame);
  boompi::audio::CapturePlanes48k output{};
  BOOMPI_EXPECT(context, mapper.Process(frame, &output).ok());
  constexpr std::size_t kProbeSample = 511U;
  for (std::size_t role_index = 0U;
       role_index < boompi::audio::kDspCaptureChannelCount; ++role_index) {
    BOOMPI_EXPECT(
        context,
        output[role_index][kProbeSample] ==
            frame.samples[kProbeSample *
                              boompi::audio::kDspCaptureChannelCount +
                          role_index]);
  }
}

void TestInvalidFramesAndOutput(boompi::test::TestContext& context) {
  auto mapper = MakeMapper(context, IdentityMap());
  auto frame = MakeFourChannelFrame();
  FillDistinctChannels(&frame);
  boompi::audio::CapturePlanes48k output{};
  output[0U][0U] = 1234;

  BOOMPI_EXPECT(context, !mapper.Process(frame, nullptr).ok());
  BOOMPI_EXPECT(context, output[0U][0U] == 1234);

  frame.format.channels = 2U;
  BOOMPI_EXPECT(context, frame.HasValidLength());
  BOOMPI_EXPECT(context, !mapper.Process(frame, &output).ok());
  BOOMPI_EXPECT(context, output[0U][0U] == 1234);

  frame = MakeFourChannelFrame();
  frame.samples_per_channel = 959U;
  BOOMPI_EXPECT(context, !mapper.Process(frame, &output).ok());
  BOOMPI_EXPECT(context, output[0U][0U] == 1234);

  frame = MakeFourChannelFrame();
  frame.format.sample_rate_hz = 16000U;
  BOOMPI_EXPECT(context, !mapper.Process(frame, &output).ok());
  BOOMPI_EXPECT(context, output[0U][0U] == 1234);

  boompi::audio::CaptureChannelMapper unconfigured;
  frame = MakeFourChannelFrame();
  BOOMPI_EXPECT(context, !unconfigured.Process(frame, &output).ok());
  BOOMPI_EXPECT(context, output[0U][0U] == 1234);
}

}  // namespace

int main() {
  boompi::test::TestContext context;
  TestIdentityAndFullFrameBoundary(context);
  TestPermutation(context);
  TestPolarityAndSaturation(context);
  TestInvalidMaps(context);
  TestFailedUpdatePreservesMapper(context);
  TestInvalidFramesAndOutput(context);

  if (context.failures() != 0U) {
    std::cerr << context.failures()
              << " audio channel mapper expectation(s) failed\n";
    return 1;
  }
  std::cout << "boomPI audio channel mapper tests passed\n";
  return 0;
}
