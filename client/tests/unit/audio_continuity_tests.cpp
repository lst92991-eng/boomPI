#include <cstdint>
#include <iostream>
#include <limits>

#include "boompi/audio/frame_continuity_gate.h"
#include "boompi/test/test_context.h"

#define BOOMPI_EXPECT(context, expression) \
  (context).Expect((expression), #expression, __FILE__, __LINE__)

namespace {

boompi::audio::AudioFrameMetadata MakeMetadata(
    const std::uint32_t epoch, const std::uint32_t stream_id,
    const std::uint32_t sequence, const std::uint64_t timestamp_us,
    const std::uint32_t turn_id = 0U, const bool discontinuity = false) {
  boompi::audio::AudioFrameMetadata metadata{};
  metadata.monotonic_timestamp_us = timestamp_us;
  metadata.sequence = sequence;
  metadata.stream_id = stream_id;
  metadata.turn_id = turn_id;
  metadata.epoch = epoch;
  metadata.discontinuity = discontinuity;
  return metadata;
}

void TestArmValidation(boompi::test::TestContext& context) {
  boompi::audio::FrameContinuityGate gate;
  BOOMPI_EXPECT(context, !gate.Arm(0U, 1U).ok());
  BOOMPI_EXPECT(context, !gate.Arm(1U, 0U).ok());
  BOOMPI_EXPECT(
      context,
      gate.CheckAndAdvance(MakeMetadata(1U, 1U, 0U, 0U)) ==
          boompi::audio::FrameContinuityResult::kNotArmed);

  BOOMPI_EXPECT(context, gate.Arm(5U, 9U).ok());
  BOOMPI_EXPECT(
      context,
      gate.CheckAndAdvance(MakeMetadata(5U, 9U, 10U, 1000U)) ==
          boompi::audio::FrameContinuityResult::kAcceptedFirst);

  // A rejected control update must not disturb the active stream.
  BOOMPI_EXPECT(context, !gate.Arm(0U, 9U).ok());
  BOOMPI_EXPECT(
      context,
      gate.CheckAndAdvance(MakeMetadata(5U, 9U, 11U, 2000U)) ==
          boompi::audio::FrameContinuityResult::kAcceptedContinuous);

  gate.Disarm();
  BOOMPI_EXPECT(
      context,
      gate.CheckAndAdvance(MakeMetadata(5U, 9U, 12U, 3000U)) ==
          boompi::audio::FrameContinuityResult::kNotArmed);
  BOOMPI_EXPECT(context, !gate.Arm(5U, 9U).ok());
  BOOMPI_EXPECT(context, gate.Arm(6U, 9U).ok());
}

void TestFirstContinuousAndTurnChanges(
    boompi::test::TestContext& context) {
  boompi::audio::FrameContinuityGate gate;
  BOOMPI_EXPECT(context, gate.Arm(3U, 7U).ok());
  BOOMPI_EXPECT(
      context,
      gate.CheckAndAdvance(MakeMetadata(3U, 7U, 100U, 10000U, 0U)) ==
          boompi::audio::FrameContinuityResult::kAcceptedFirst);
  BOOMPI_EXPECT(
      context,
      gate.CheckAndAdvance(MakeMetadata(3U, 7U, 101U, 30000U, 42U)) ==
          boompi::audio::FrameContinuityResult::kAcceptedContinuous);
  BOOMPI_EXPECT(
      context,
      gate.CheckAndAdvance(MakeMetadata(3U, 7U, 102U, 50000U, 99U)) ==
          boompi::audio::FrameContinuityResult::kAcceptedContinuous);
}

void TestMismatchedFramesDoNotPollute(
    boompi::test::TestContext& context) {
  boompi::audio::FrameContinuityGate gate;
  BOOMPI_EXPECT(context, gate.Arm(8U, 12U).ok());
  BOOMPI_EXPECT(
      context,
      gate.CheckAndAdvance(MakeMetadata(8U, 12U, 20U, 1000U)) ==
          boompi::audio::FrameContinuityResult::kAcceptedFirst);

  BOOMPI_EXPECT(
      context,
      gate.CheckAndAdvance(MakeMetadata(7U, 12U, 900U, 0U, 0U, true)) ==
          boompi::audio::FrameContinuityResult::kEpochMismatch);
  BOOMPI_EXPECT(
      context,
      gate.CheckAndAdvance(MakeMetadata(8U, 13U, 900U, 0U, 0U, true)) ==
          boompi::audio::FrameContinuityResult::kStreamMismatch);
  BOOMPI_EXPECT(
      context,
      gate.CheckAndAdvance(MakeMetadata(8U, 12U, 21U, 2000U)) ==
          boompi::audio::FrameContinuityResult::kAcceptedContinuous);
}

void TestSequenceWrap(boompi::test::TestContext& context) {
  boompi::audio::FrameContinuityGate gate;
  BOOMPI_EXPECT(context, gate.Arm(1U, 1U).ok());
  BOOMPI_EXPECT(
      context,
      gate.CheckAndAdvance(
          MakeMetadata(1U, 1U, std::numeric_limits<std::uint32_t>::max(),
                       1000U)) ==
          boompi::audio::FrameContinuityResult::kAcceptedFirst);
  BOOMPI_EXPECT(
      context,
      gate.CheckAndAdvance(MakeMetadata(1U, 1U, 0U, 2000U)) ==
          boompi::audio::FrameContinuityResult::kSequenceBreak);

  boompi::audio::FrameContinuityGate explicit_gate;
  BOOMPI_EXPECT(context, explicit_gate.Arm(1U, 1U).ok());
  BOOMPI_EXPECT(
      context,
      explicit_gate.CheckAndAdvance(
          MakeMetadata(1U, 1U, std::numeric_limits<std::uint32_t>::max(),
                       1000U)) ==
          boompi::audio::FrameContinuityResult::kAcceptedFirst);
  BOOMPI_EXPECT(
      context,
      explicit_gate.CheckAndAdvance(
          MakeMetadata(1U, 1U, 0U, 2000U, 0U, true)) ==
          boompi::audio::FrameContinuityResult::kDiscontinuity);
}

void TestExplicitDiscontinuityLatchesFault(
    boompi::test::TestContext& context) {
  boompi::audio::FrameContinuityGate gate;
  BOOMPI_EXPECT(context, gate.Arm(2U, 4U).ok());
  BOOMPI_EXPECT(
      context,
      gate.CheckAndAdvance(MakeMetadata(2U, 4U, 1U, 1000U)) ==
          boompi::audio::FrameContinuityResult::kAcceptedFirst);
  BOOMPI_EXPECT(
      context,
      gate.CheckAndAdvance(MakeMetadata(2U, 4U, 2U, 2000U, 0U, true)) ==
          boompi::audio::FrameContinuityResult::kDiscontinuity);
  BOOMPI_EXPECT(
      context,
      gate.CheckAndAdvance(MakeMetadata(2U, 4U, 2U, 2000U)) ==
          boompi::audio::FrameContinuityResult::kFaulted);

  // Mismatched queued frames remain harmless even while the active flow is
  // faulted, and they cannot clear that fault.
  BOOMPI_EXPECT(
      context,
      gate.CheckAndAdvance(MakeMetadata(1U, 4U, 2U, 2000U)) ==
          boompi::audio::FrameContinuityResult::kEpochMismatch);
  BOOMPI_EXPECT(
      context,
      gate.CheckAndAdvance(MakeMetadata(2U, 5U, 2U, 2000U)) ==
          boompi::audio::FrameContinuityResult::kStreamMismatch);
  BOOMPI_EXPECT(
      context,
      gate.CheckAndAdvance(MakeMetadata(2U, 4U, 2U, 2000U)) ==
          boompi::audio::FrameContinuityResult::kFaulted);

  BOOMPI_EXPECT(context, !gate.Arm(2U, 4U).ok());
  BOOMPI_EXPECT(
      context,
      gate.CheckAndAdvance(MakeMetadata(2U, 4U, 500U, 9000U)) ==
          boompi::audio::FrameContinuityResult::kFaulted);
  gate.Disarm();
  BOOMPI_EXPECT(context, !gate.Arm(2U, 4U).ok());
  BOOMPI_EXPECT(
      context,
      gate.CheckAndAdvance(MakeMetadata(2U, 4U, 500U, 9000U)) ==
          boompi::audio::FrameContinuityResult::kNotArmed);
  BOOMPI_EXPECT(context, gate.Arm(3U, 4U).ok());
  BOOMPI_EXPECT(
      context,
      gate.CheckAndAdvance(MakeMetadata(3U, 4U, 500U, 9000U)) ==
          boompi::audio::FrameContinuityResult::kAcceptedFirst);
}

void TestSequenceBreaksLatchFault(boompi::test::TestContext& context) {
  boompi::audio::FrameContinuityGate gap_gate;
  BOOMPI_EXPECT(context, gap_gate.Arm(1U, 2U).ok());
  BOOMPI_EXPECT(
      context,
      gap_gate.CheckAndAdvance(MakeMetadata(1U, 2U, 10U, 1000U)) ==
          boompi::audio::FrameContinuityResult::kAcceptedFirst);
  BOOMPI_EXPECT(
      context,
      gap_gate.CheckAndAdvance(MakeMetadata(1U, 2U, 12U, 2000U)) ==
          boompi::audio::FrameContinuityResult::kSequenceBreak);
  BOOMPI_EXPECT(
      context,
      gap_gate.CheckAndAdvance(MakeMetadata(1U, 2U, 11U, 3000U)) ==
          boompi::audio::FrameContinuityResult::kFaulted);

  boompi::audio::FrameContinuityGate duplicate_gate;
  BOOMPI_EXPECT(context, duplicate_gate.Arm(1U, 3U).ok());
  BOOMPI_EXPECT(
      context,
      duplicate_gate.CheckAndAdvance(MakeMetadata(1U, 3U, 10U, 1000U)) ==
          boompi::audio::FrameContinuityResult::kAcceptedFirst);
  BOOMPI_EXPECT(
      context,
      duplicate_gate.CheckAndAdvance(MakeMetadata(1U, 3U, 10U, 2000U)) ==
          boompi::audio::FrameContinuityResult::kSequenceBreak);
}

void TestTimestampFailuresLatchFault(boompi::test::TestContext& context) {
  boompi::audio::FrameContinuityGate equal_gate;
  BOOMPI_EXPECT(context, equal_gate.Arm(6U, 1U).ok());
  BOOMPI_EXPECT(
      context,
      equal_gate.CheckAndAdvance(MakeMetadata(6U, 1U, 1U, 5000U)) ==
          boompi::audio::FrameContinuityResult::kAcceptedFirst);
  BOOMPI_EXPECT(
      context,
      equal_gate.CheckAndAdvance(MakeMetadata(6U, 1U, 2U, 5000U)) ==
          boompi::audio::FrameContinuityResult::kTimestampNotIncreasing);
  BOOMPI_EXPECT(
      context,
      equal_gate.CheckAndAdvance(MakeMetadata(6U, 1U, 2U, 6000U)) ==
          boompi::audio::FrameContinuityResult::kFaulted);

  boompi::audio::FrameContinuityGate regression_gate;
  BOOMPI_EXPECT(context, regression_gate.Arm(6U, 2U).ok());
  BOOMPI_EXPECT(
      context,
      regression_gate.CheckAndAdvance(MakeMetadata(6U, 2U, 1U, 5000U)) ==
          boompi::audio::FrameContinuityResult::kAcceptedFirst);
  BOOMPI_EXPECT(
      context,
      regression_gate.CheckAndAdvance(MakeMetadata(6U, 2U, 2U, 4999U)) ==
          boompi::audio::FrameContinuityResult::kTimestampNotIncreasing);
}

void TestRearmChangesGeneration(boompi::test::TestContext& context) {
  boompi::audio::FrameContinuityGate gate;
  BOOMPI_EXPECT(context, gate.Arm(10U, 20U).ok());
  BOOMPI_EXPECT(
      context,
      gate.CheckAndAdvance(MakeMetadata(10U, 20U, 1U, 1000U)) ==
          boompi::audio::FrameContinuityResult::kAcceptedFirst);
  BOOMPI_EXPECT(
      context,
      gate.CheckAndAdvance(MakeMetadata(10U, 20U, 9U, 2000U)) ==
          boompi::audio::FrameContinuityResult::kSequenceBreak);

  BOOMPI_EXPECT(context, gate.Arm(11U, 21U).ok());
  BOOMPI_EXPECT(
      context,
      gate.CheckAndAdvance(MakeMetadata(10U, 20U, 2U, 3000U)) ==
          boompi::audio::FrameContinuityResult::kEpochMismatch);
  BOOMPI_EXPECT(
      context,
      gate.CheckAndAdvance(MakeMetadata(11U, 21U, 123U, 4000U)) ==
          boompi::audio::FrameContinuityResult::kAcceptedFirst);
}

}  // namespace

int main() {
  boompi::test::TestContext context;
  TestArmValidation(context);
  TestFirstContinuousAndTurnChanges(context);
  TestMismatchedFramesDoNotPollute(context);
  TestSequenceWrap(context);
  TestExplicitDiscontinuityLatchesFault(context);
  TestSequenceBreaksLatchFault(context);
  TestTimestampFailuresLatchFault(context);
  TestRearmChangesGeneration(context);

  if (context.failures() != 0U) {
    std::cerr << context.failures()
              << " audio continuity expectation(s) failed\n";
    return 1;
  }
  std::cout << "boomPI audio continuity tests passed\n";
  return 0;
}
