#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <new>
#include <type_traits>
#include <utility>

#include "boompi/audio/vad_utterance_controller.h"
#include "boompi/test/test_context.h"

#define BOOMPI_EXPECT(context, expression) \
  (context).Expect((expression), #expression, __FILE__, __LINE__)

namespace {

bool g_track_allocations = false;
std::size_t g_allocation_count = 0U;

}  // namespace

void* operator new(const std::size_t size) {
  if (g_track_allocations) {
    ++g_allocation_count;
  }
  if (void* const storage = std::malloc(size)) {
    return storage;
  }
  throw std::bad_alloc();
}

void* operator new[](const std::size_t size) {
  return ::operator new(size);
}

void operator delete(void* const storage) noexcept { std::free(storage); }

void operator delete[](void* const storage) noexcept {
  ::operator delete(storage);
}

void operator delete(void* const storage, std::size_t) noexcept {
  std::free(storage);
}

void operator delete[](void* const storage, std::size_t) noexcept {
  ::operator delete(storage);
}

namespace {

using boompi::StatusCode;
using boompi::audio::HasVadControlIntent;
using boompi::audio::Mono16kFrame;
using boompi::audio::SampleFormat;
using boompi::audio::VadControlIntent;
using boompi::audio::VadControllerCode;
using boompi::audio::VadControllerResult;
using boompi::audio::VadObservation;
using boompi::audio::VadUtteranceController;
using boompi::audio::VadUtteranceEndReason;

static_assert(std::is_trivial<VadControllerResult>::value,
              "VAD result must remain trivial");
static_assert(std::is_standard_layout<VadControllerResult>::value,
              "VAD result must remain standard-layout");
static_assert(
    noexcept(std::declval<VadUtteranceController&>().Process(
        std::declval<const Mono16kFrame&>(), VadObservation::kSilence)),
    "VAD hot-path processing must remain noexcept");
static_assert(
    noexcept(std::declval<VadUtteranceController&>().TryPopUplinkFrame(
        std::declval<Mono16kFrame*>())),
    "VAD hot-path output must remain noexcept");
static_assert(boompi::audio::kVadPreRollFrameCount == 25U,
              "500 ms pre-roll requires 25 fixed frames");
static_assert(boompi::audio::kVadUplinkQueueCapacity == 40U,
              "uplink buffering must remain bounded to 800 ms");

Mono16kFrame MakeFrame(const std::uint32_t epoch,
                       const std::uint32_t stream_id,
                       const std::uint32_t sequence,
                       const std::uint64_t timestamp_us = 0U,
                       const bool discontinuity = false) {
  Mono16kFrame frame{};
  frame.format = {16000U, 20U, 1U, SampleFormat::kPcmS16Le};
  frame.metadata.monotonic_timestamp_us =
      timestamp_us == 0U
          ? (static_cast<std::uint64_t>(sequence) + 1U) * 20000U
          : timestamp_us;
  frame.metadata.sequence = sequence;
  frame.metadata.stream_id = stream_id;
  frame.metadata.turn_id = 0U;
  frame.metadata.epoch = epoch;
  frame.metadata.discontinuity = discontinuity;
  frame.samples_per_channel = 320U;
  frame.samples.fill(static_cast<std::int16_t>(sequence & 0x7FFFU));
  return frame;
}

bool HasIntent(const VadControllerResult& result,
               const VadControlIntent intent) {
  return HasVadControlIntent(result.intents, intent);
}

void ExpectAccepted(boompi::test::TestContext& context,
                    const VadControllerResult& result) {
  BOOMPI_EXPECT(context, result.valid());
  BOOMPI_EXPECT(context, result.code == VadControllerCode::kAccepted);
  BOOMPI_EXPECT(context,
                result.end_reason == VadUtteranceEndReason::kNone);
}

void FillPreRoll(boompi::test::TestContext& context,
                 VadUtteranceController& controller,
                 const std::uint32_t epoch, const std::uint32_t stream_id,
                 const std::uint32_t first_sequence = 0U,
                 const std::uint32_t count = 25U) {
  for (std::uint32_t offset = 0U; offset < count; ++offset) {
    const auto result = controller.Process(
        MakeFrame(epoch, stream_id, first_sequence + offset),
        VadObservation::kSilence);
    ExpectAccepted(context, result);
  }
}

void DrainAndExpectSequence(boompi::test::TestContext& context,
                            VadUtteranceController& controller,
                            const std::uint32_t first_sequence,
                            const std::uint32_t count,
                            const std::uint32_t turn_id) {
  Mono16kFrame output{};
  for (std::uint32_t offset = 0U; offset < count; ++offset) {
    BOOMPI_EXPECT(context, controller.TryPopUplinkFrame(&output));
    BOOMPI_EXPECT(context,
                  output.metadata.sequence == first_sequence + offset);
    BOOMPI_EXPECT(context, output.metadata.turn_id == turn_id);
    BOOMPI_EXPECT(context,
                  output.samples.front() == static_cast<std::int16_t>(
                                                (first_sequence + offset) &
                                                0x7FFFU));
    BOOMPI_EXPECT(context, output.HasValidLength());
  }
}

void TestCommandValidation(boompi::test::TestContext& context) {
  VadUtteranceController controller;
  auto status = controller.BeginListening(1U, 0U);
  BOOMPI_EXPECT(context, !status.ok());
  BOOMPI_EXPECT(context, status.code() == StatusCode::kFailedPrecondition);

  status = controller.Arm(0U, 1U);
  BOOMPI_EXPECT(context, !status.ok());
  BOOMPI_EXPECT(context, controller.faulted());

  status = controller.Arm(1U, 2U);
  BOOMPI_EXPECT(context, status.ok());
  BOOMPI_EXPECT(context, controller.armed());

  status = controller.BeginListening(0U, 0U);
  BOOMPI_EXPECT(context, !status.ok());
  BOOMPI_EXPECT(context, status.code() == StatusCode::kInvalidArgument);

  status = controller.BeginPlaybackBargeIn(4U, 4U);
  BOOMPI_EXPECT(context, !status.ok());
  BOOMPI_EXPECT(context, status.code() == StatusCode::kInvalidArgument);

  status = controller.BeginListening(
      4U, UINT64_MAX -
              static_cast<std::uint64_t>(
                  boompi::audio::kVadFirstSpeechTimeoutMs) *
                  1000U +
              1U);
  BOOMPI_EXPECT(context, !status.ok());
  BOOMPI_EXPECT(context, status.code() == StatusCode::kInvalidArgument);
}

void TestPreRollPreservesSentenceStartAndWrap(
    boompi::test::TestContext& context) {
  VadUtteranceController controller;
  BOOMPI_EXPECT(context, controller.Arm(1U, 2U).ok());
  FillPreRoll(context, controller, 1U, 2U, 0U, 40U);

  BOOMPI_EXPECT(context, controller.BeginListening(77U, 800000U).ok());
  const auto started = controller.Process(MakeFrame(1U, 2U, 40U),
                                          VadObservation::kSpeech);
  BOOMPI_EXPECT(context,
                started.code == VadControllerCode::kUtteranceStarted);
  BOOMPI_EXPECT(context,
                HasIntent(started, VadControlIntent::kStartUtterance));
  BOOMPI_EXPECT(context, started.queued_uplink_frames == 25U);

  // The 25-frame snapshot is the exact 500 ms ending in the first speech
  // frame. Ring wrap must not reorder or lose the beginning of the sentence.
  DrainAndExpectSequence(context, controller, 16U, 25U, 77U);
  BOOMPI_EXPECT(context, controller.queued_uplink_frames() == 0U);
  Mono16kFrame output{};
  BOOMPI_EXPECT(context, !controller.TryPopUplinkFrame(&output));
}

void TestStaleEpochNeverEntersPreRoll(
    boompi::test::TestContext& context) {
  VadUtteranceController controller;
  BOOMPI_EXPECT(context, controller.Arm(5U, 6U).ok());
  FillPreRoll(context, controller, 5U, 6U, 0U, 24U);

  // Continuity rejects an old generation before any detector result is
  // consumed; even an unset observation on that stale frame cannot fault the
  // current generation.
  const auto stale = controller.Process(MakeFrame(4U, 6U, 999U),
                                        VadObservation::kUnset);
  BOOMPI_EXPECT(context,
                stale.code == VadControllerCode::kStaleFrameDropped);
  BOOMPI_EXPECT(context, controller.armed());
  BOOMPI_EXPECT(context, !controller.faulted());

  ExpectAccepted(context,
                 controller.Process(MakeFrame(5U, 6U, 24U),
                                    VadObservation::kSilence));
  BOOMPI_EXPECT(context, controller.BeginListening(88U, 500000U).ok());
  const auto started = controller.Process(MakeFrame(5U, 6U, 25U),
                                          VadObservation::kSpeech);
  BOOMPI_EXPECT(context,
                started.code == VadControllerCode::kUtteranceStarted);
  DrainAndExpectSequence(context, controller, 1U, 25U, 88U);
}

void TestDiscontinuityFlushesAndRequiresNewGeneration(
    boompi::test::TestContext& context) {
  VadUtteranceController controller;
  BOOMPI_EXPECT(context, controller.Arm(10U, 11U).ok());
  FillPreRoll(context, controller, 10U, 11U);
  BOOMPI_EXPECT(context, controller.BeginListening(12U, 500000U).ok());

  const auto fault = controller.Process(
      MakeFrame(10U, 11U, 25U, 520000U, true), VadObservation::kSpeech);
  BOOMPI_EXPECT(context, fault.code == VadControllerCode::kFaulted);
  BOOMPI_EXPECT(context,
                fault.end_reason == VadUtteranceEndReason::kContinuityFault);
  BOOMPI_EXPECT(context,
                HasIntent(fault, VadControlIntent::kCancelUserTurn));
  BOOMPI_EXPECT(context, controller.queued_uplink_frames() == 0U);
  BOOMPI_EXPECT(context, !controller.armed());
  BOOMPI_EXPECT(context, controller.faulted());

  auto status = controller.Arm(10U, 11U);
  BOOMPI_EXPECT(context, !status.ok());
  status = controller.Arm(9U, 12U);
  BOOMPI_EXPECT(context, !status.ok());
  status = controller.Arm(11U, 11U);
  BOOMPI_EXPECT(context, status.ok());
}

void TestFirstSpeechTimeoutBoundary(boompi::test::TestContext& context) {
  constexpr std::uint64_t kCommandTimestampUs = 500000U;
  constexpr std::uint64_t kDeadlineUs = 6500000U;

  VadUtteranceController before_deadline;
  BOOMPI_EXPECT(context, before_deadline.Arm(1U, 2U).ok());
  FillPreRoll(context, before_deadline, 1U, 2U);
  BOOMPI_EXPECT(
      context,
      before_deadline.BeginListening(3U, kCommandTimestampUs).ok());
  const auto accepted = before_deadline.Process(
      MakeFrame(1U, 2U, 25U, kDeadlineUs - 1U),
      VadObservation::kSpeech);
  BOOMPI_EXPECT(context,
                accepted.code == VadControllerCode::kUtteranceStarted);

  VadUtteranceController at_deadline;
  BOOMPI_EXPECT(context, at_deadline.Arm(4U, 5U).ok());
  FillPreRoll(context, at_deadline, 4U, 5U);
  BOOMPI_EXPECT(context,
                at_deadline.BeginListening(6U, kCommandTimestampUs).ok());
  const auto timed_out = at_deadline.Process(
      MakeFrame(4U, 5U, 25U, kDeadlineUs), VadObservation::kSpeech);
  BOOMPI_EXPECT(context,
                timed_out.code == VadControllerCode::kListeningTimedOut);
  BOOMPI_EXPECT(
      context,
      timed_out.end_reason == VadUtteranceEndReason::kFirstSpeechTimeout);
  BOOMPI_EXPECT(context,
                HasIntent(timed_out, VadControlIntent::kCancelUserTurn));
  BOOMPI_EXPECT(context, at_deadline.queued_uplink_frames() == 0U);
  BOOMPI_EXPECT(context, at_deadline.armed());
}

void TestTrailingSilenceBoundary(boompi::test::TestContext& context) {
  VadUtteranceController controller;
  BOOMPI_EXPECT(context, controller.Arm(1U, 2U).ok());
  FillPreRoll(context, controller, 1U, 2U);
  BOOMPI_EXPECT(context, controller.BeginListening(3U, 500000U).ok());
  BOOMPI_EXPECT(context,
                controller.Process(MakeFrame(1U, 2U, 25U),
                                   VadObservation::kSpeech)
                        .code == VadControllerCode::kUtteranceStarted);
  DrainAndExpectSequence(context, controller, 1U, 25U, 3U);

  Mono16kFrame output{};
  for (std::uint32_t offset = 0U; offset < 34U; ++offset) {
    const auto result = controller.Process(MakeFrame(1U, 2U, 26U + offset),
                                           VadObservation::kSilence);
    ExpectAccepted(context, result);
    BOOMPI_EXPECT(context, controller.TryPopUplinkFrame(&output));
  }
  const auto ended = controller.Process(MakeFrame(1U, 2U, 60U),
                                        VadObservation::kSilence);
  BOOMPI_EXPECT(context,
                ended.code == VadControllerCode::kUtteranceEnded);
  BOOMPI_EXPECT(
      context,
      ended.end_reason == VadUtteranceEndReason::kTrailingSilence);
  BOOMPI_EXPECT(context,
                HasIntent(ended, VadControlIntent::kEndUtterance));
  BOOMPI_EXPECT(context, ended.user_turn_id == 3U);
  BOOMPI_EXPECT(context, controller.queued_uplink_frames() == 1U);

  // A normal end is announced after the tail frame is queued. Cancelling
  // before that tail drains must retain and cancel the downstream user turn.
  // A late playback-end ACK must not erase that pending identity either.
  const auto playback_ended = controller.EndPlayback();
  BOOMPI_EXPECT(context, playback_ended.user_turn_id == 3U);
  const auto cancelled = controller.Cancel();
  BOOMPI_EXPECT(context, cancelled.user_turn_id == 3U);
  BOOMPI_EXPECT(context,
                HasIntent(cancelled, VadControlIntent::kCancelUserTurn));
  BOOMPI_EXPECT(context, controller.queued_uplink_frames() == 0U);
  BOOMPI_EXPECT(context, !controller.armed());
}

void TestEndedTailDiscontinuityCancelsPendingTurn(
    boompi::test::TestContext& context) {
  VadUtteranceController controller;
  BOOMPI_EXPECT(context, controller.Arm(1U, 2U).ok());
  FillPreRoll(context, controller, 1U, 2U);
  BOOMPI_EXPECT(context, controller.BeginListening(33U, 500000U).ok());
  BOOMPI_EXPECT(context,
                controller.Process(MakeFrame(1U, 2U, 25U),
                                   VadObservation::kSpeech)
                        .code == VadControllerCode::kUtteranceStarted);
  DrainAndExpectSequence(context, controller, 1U, 25U, 33U);

  Mono16kFrame output{};
  for (std::uint32_t offset = 0U; offset < 34U; ++offset) {
    ExpectAccepted(context,
                   controller.Process(MakeFrame(1U, 2U, 26U + offset),
                                      VadObservation::kSilence));
    BOOMPI_EXPECT(context, controller.TryPopUplinkFrame(&output));
  }
  const auto ended = controller.Process(MakeFrame(1U, 2U, 60U),
                                        VadObservation::kSilence);
  BOOMPI_EXPECT(context,
                ended.code == VadControllerCode::kUtteranceEnded);
  BOOMPI_EXPECT(context, controller.queued_uplink_frames() == 1U);

  const auto fault = controller.Process(
      MakeFrame(1U, 2U, 61U, 1240000U, true), VadObservation::kSilence);
  BOOMPI_EXPECT(context, fault.code == VadControllerCode::kFaulted);
  BOOMPI_EXPECT(context, fault.user_turn_id == 33U);
  BOOMPI_EXPECT(context,
                HasIntent(fault, VadControlIntent::kCancelUserTurn));
  BOOMPI_EXPECT(context, controller.queued_uplink_frames() == 0U);
  BOOMPI_EXPECT(context, !controller.armed());
  BOOMPI_EXPECT(context, controller.faulted());
}

void TestMaximumUtteranceBoundary(boompi::test::TestContext& context) {
  VadUtteranceController controller;
  BOOMPI_EXPECT(context, controller.Arm(1U, 2U).ok());
  FillPreRoll(context, controller, 1U, 2U);
  BOOMPI_EXPECT(context, controller.BeginListening(3U, 500000U).ok());
  BOOMPI_EXPECT(context,
                controller.Process(MakeFrame(1U, 2U, 25U),
                                   VadObservation::kSpeech)
                        .code == VadControllerCode::kUtteranceStarted);
  DrainAndExpectSequence(context, controller, 1U, 25U, 3U);

  Mono16kFrame output{};
  for (std::uint32_t utterance_frame = 2U;
       utterance_frame <
       boompi::audio::kVadMaximumUtteranceFrameCount;
       ++utterance_frame) {
    const std::uint32_t sequence = 24U + utterance_frame;
    const auto result = controller.Process(MakeFrame(1U, 2U, sequence),
                                           VadObservation::kSpeech);
    ExpectAccepted(context, result);
    BOOMPI_EXPECT(context, controller.TryPopUplinkFrame(&output));
  }

  const std::uint32_t final_sequence =
      24U + boompi::audio::kVadMaximumUtteranceFrameCount;
  const auto ended = controller.Process(MakeFrame(1U, 2U, final_sequence),
                                        VadObservation::kSpeech);
  BOOMPI_EXPECT(context,
                ended.code == VadControllerCode::kUtteranceEnded);
  BOOMPI_EXPECT(
      context,
      ended.end_reason == VadUtteranceEndReason::kMaximumDuration);
  BOOMPI_EXPECT(context, controller.TryPopUplinkFrame(&output));
  BOOMPI_EXPECT(context, output.metadata.sequence == final_sequence);
}

void TestBargeInDuckConfirmAndSentenceStart(
    boompi::test::TestContext& context) {
  VadUtteranceController controller;
  BOOMPI_EXPECT(context, controller.Arm(1U, 2U).ok());
  FillPreRoll(context, controller, 1U, 2U);
  BOOMPI_EXPECT(context, controller.BeginPlaybackBargeIn(100U, 101U).ok());

  for (std::uint32_t sequence = 25U; sequence < 28U; ++sequence) {
    const auto result = controller.Process(MakeFrame(1U, 2U, sequence),
                                           VadObservation::kSpeech);
    ExpectAccepted(context, result);
    BOOMPI_EXPECT(context,
                  !HasIntent(result, VadControlIntent::kDuckPlayback));
  }
  const auto duck = controller.Process(MakeFrame(1U, 2U, 28U),
                                       VadObservation::kSpeech);
  BOOMPI_EXPECT(context, HasIntent(duck, VadControlIntent::kDuckPlayback));

  const auto false_candidate = controller.Process(MakeFrame(1U, 2U, 29U),
                                                  VadObservation::kSilence);
  BOOMPI_EXPECT(
      context,
      HasIntent(false_candidate, VadControlIntent::kRestorePlayback));

  VadControllerResult confirmed{};
  for (std::uint32_t offset = 0U; offset < 8U; ++offset) {
    const auto result = controller.Process(MakeFrame(1U, 2U, 30U + offset),
                                           VadObservation::kSpeech);
    if (offset == 3U) {
      BOOMPI_EXPECT(context,
                    HasIntent(result, VadControlIntent::kDuckPlayback));
    }
    if (offset == 7U) {
      confirmed = result;
    }
  }

  BOOMPI_EXPECT(context,
                confirmed.code == VadControllerCode::kUtteranceStarted);
  BOOMPI_EXPECT(
      context,
      HasIntent(confirmed, VadControlIntent::kInterruptConfirmed));
  BOOMPI_EXPECT(context,
                HasIntent(confirmed, VadControlIntent::kCancelResponse));
  BOOMPI_EXPECT(
      context,
      HasIntent(confirmed, VadControlIntent::kClearPlaybackQueue));
  BOOMPI_EXPECT(context,
                HasIntent(confirmed, VadControlIntent::kStartUtterance));
  BOOMPI_EXPECT(context, confirmed.response_turn_id == 100U);
  BOOMPI_EXPECT(context, confirmed.user_turn_id == 101U);
  DrainAndExpectSequence(context, controller, 13U, 25U, 101U);
}

void TestCancelEmitsAtomicPlaybackCleanup(
    boompi::test::TestContext& context) {
  VadUtteranceController controller;
  BOOMPI_EXPECT(context, controller.Arm(1U, 2U).ok());
  FillPreRoll(context, controller, 1U, 2U);
  BOOMPI_EXPECT(context, controller.BeginPlaybackBargeIn(20U, 21U).ok());
  for (std::uint32_t sequence = 25U; sequence < 29U; ++sequence) {
    controller.Process(MakeFrame(1U, 2U, sequence),
                       VadObservation::kSpeech);
  }

  const auto cancelled = controller.Cancel();
  BOOMPI_EXPECT(context,
                cancelled.code == VadControllerCode::kCancelled);
  BOOMPI_EXPECT(context,
                HasIntent(cancelled, VadControlIntent::kCancelResponse));
  BOOMPI_EXPECT(
      context,
      HasIntent(cancelled, VadControlIntent::kClearPlaybackQueue));
  BOOMPI_EXPECT(context,
                HasIntent(cancelled, VadControlIntent::kRestorePlayback));
  BOOMPI_EXPECT(context,
                HasIntent(cancelled, VadControlIntent::kCancelUserTurn));
  BOOMPI_EXPECT(context, cancelled.queued_uplink_frames == 0U);
  BOOMPI_EXPECT(context, !controller.armed());
  BOOMPI_EXPECT(context, controller.queued_uplink_frames() == 0U);
}

void TestOutputOverflowFailsClosed(boompi::test::TestContext& context) {
  VadUtteranceController controller;
  BOOMPI_EXPECT(context, controller.Arm(1U, 2U).ok());
  FillPreRoll(context, controller, 1U, 2U);
  BOOMPI_EXPECT(context, controller.BeginListening(3U, 500000U).ok());
  BOOMPI_EXPECT(context,
                controller.Process(MakeFrame(1U, 2U, 25U),
                                   VadObservation::kSpeech)
                        .code == VadControllerCode::kUtteranceStarted);

  for (std::uint32_t sequence = 26U; sequence < 41U; ++sequence) {
    ExpectAccepted(context,
                   controller.Process(MakeFrame(1U, 2U, sequence),
                                      VadObservation::kSpeech));
  }
  BOOMPI_EXPECT(context,
                controller.queued_uplink_frames() ==
                    boompi::audio::kVadUplinkQueueCapacity);

  const auto overflow = controller.Process(MakeFrame(1U, 2U, 41U),
                                           VadObservation::kSpeech);
  BOOMPI_EXPECT(context, overflow.code == VadControllerCode::kFaulted);
  BOOMPI_EXPECT(context,
                overflow.end_reason == VadUtteranceEndReason::kOutputOverflow);
  BOOMPI_EXPECT(context,
                HasIntent(overflow, VadControlIntent::kCancelUserTurn));
  BOOMPI_EXPECT(context, controller.queued_uplink_frames() == 0U);
  BOOMPI_EXPECT(context, !controller.armed());
}

void TestInvalidObservationFailsClosed(
    boompi::test::TestContext& context) {
  VadUtteranceController controller;
  BOOMPI_EXPECT(context, controller.Arm(1U, 2U).ok());
  const auto fault = controller.Process(MakeFrame(1U, 2U, 0U),
                                        VadObservation::kUnset);
  BOOMPI_EXPECT(context, fault.code == VadControllerCode::kFaulted);
  BOOMPI_EXPECT(context,
                fault.end_reason == VadUtteranceEndReason::kInvalidInput);
  BOOMPI_EXPECT(context, controller.queued_uplink_frames() == 0U);
}

void TestHotPathDoesNotAllocate(boompi::test::TestContext& context) {
  VadUtteranceController controller;
  BOOMPI_EXPECT(context, controller.Arm(1U, 2U).ok());
  const Mono16kFrame frame = MakeFrame(1U, 2U, 0U);

  g_allocation_count = 0U;
  g_track_allocations = true;
  const auto result = controller.Process(frame, VadObservation::kSilence);
  g_track_allocations = false;

  ExpectAccepted(context, result);
  BOOMPI_EXPECT(context, g_allocation_count == 0U);
}

}  // namespace

int main() {
  boompi::test::TestContext context;
  TestCommandValidation(context);
  TestPreRollPreservesSentenceStartAndWrap(context);
  TestStaleEpochNeverEntersPreRoll(context);
  TestDiscontinuityFlushesAndRequiresNewGeneration(context);
  TestFirstSpeechTimeoutBoundary(context);
  TestTrailingSilenceBoundary(context);
  TestEndedTailDiscontinuityCancelsPendingTurn(context);
  TestMaximumUtteranceBoundary(context);
  TestBargeInDuckConfirmAndSentenceStart(context);
  TestCancelEmitsAtomicPlaybackCleanup(context);
  TestOutputOverflowFailsClosed(context);
  TestInvalidObservationFailsClosed(context);
  TestHotPathDoesNotAllocate(context);

  if (context.failures() != 0U) {
    std::cerr << context.failures()
              << " VAD utterance controller expectation(s) failed\n";
    return 1;
  }
  std::cout << "VAD utterance controller tests passed\n";
  return 0;
}
