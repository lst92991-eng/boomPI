#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <type_traits>
#include <utility>

#include "boompi/audio/pcm_playback_sink.h"
#include "boompi/test/scripted_pcm_playback_sink.h"
#include "boompi/test/test_context.h"

#define BOOMPI_EXPECT(context, expression) \
  (context).Expect((expression), #expression, __FILE__, __LINE__)

namespace {

using boompi::audio::PcmPlaybackControlCode;
using boompi::audio::PcmPlaybackControlResult;
using boompi::audio::PcmPlaybackSink;
using boompi::audio::PcmPlaybackWriteCode;
using boompi::audio::PcmPlaybackWriteResult;
using boompi::audio::PlaybackTimingSource;
using boompi::audio::UnavailablePcmPlaybackSink;
using boompi::test::ScriptedPcmPlaybackCall;
using boompi::test::ScriptedPcmPlaybackSink;

static_assert(std::is_trivial<PcmPlaybackWriteResult>::value,
              "write result must remain a POD value");
static_assert(std::is_standard_layout<PcmPlaybackWriteResult>::value,
              "write result must remain standard-layout");
static_assert(std::is_trivial<PcmPlaybackControlResult>::value,
              "control result must remain a POD value");
static_assert(std::is_standard_layout<PcmPlaybackControlResult>::value,
              "control result must remain standard-layout");
static_assert(
    noexcept(std::declval<PcmPlaybackSink&>().TryWriteInterleavedS16(
        std::declval<const std::int16_t*>(), std::uint16_t{})),
    "sink write boundary must remain noexcept");
static_assert(noexcept(std::declval<PcmPlaybackSink&>().Drop()),
              "sink drop boundary must remain noexcept");
static_assert(noexcept(std::declval<PcmPlaybackSink&>().Prepare()),
              "sink prepare boundary must remain noexcept");

constexpr PcmPlaybackWriteResult EmptyWriteResult(
    const PcmPlaybackWriteCode code,
    const std::int32_t native_error = 0) noexcept {
  return {code, 0U, 0U, 0U, 0U, PlaybackTimingSource::kUnset,
          native_error};
}

constexpr PcmPlaybackWriteResult AcceptedWriteResult(
    const std::uint16_t frames = 17U,
    const std::uint64_t write_timestamp_us = 1000U,
    const std::uint64_t presentation_timestamp_us = 2000U,
    const std::uint32_t queued_frames = 48U,
    const PlaybackTimingSource timing_source =
        PlaybackTimingSource::kSoftwareEstimate) noexcept {
  return {PcmPlaybackWriteCode::kAccepted, frames, write_timestamp_us,
          presentation_timestamp_us, queued_frames, timing_source, 0};
}

void TestWriteResultContract(boompi::test::TestContext& context) {
  constexpr auto accepted = AcceptedWriteResult();
  static_assert(accepted.valid() && accepted.accepted(),
                "well-formed accepted result must be valid");
  BOOMPI_EXPECT(context, accepted.valid());
  BOOMPI_EXPECT(context, accepted.accepted());
  BOOMPI_EXPECT(context, !accepted.retryable_without_reset());
  BOOMPI_EXPECT(context, !accepted.requires_stream_reset());

  auto invalid = accepted;
  invalid.accepted_sample_frames = 0U;
  BOOMPI_EXPECT(context, !invalid.valid());
  invalid = accepted;
  invalid.write_completed_timestamp_us = 0U;
  BOOMPI_EXPECT(context, !invalid.valid());
  invalid = accepted;
  invalid.estimated_presentation_timestamp_us = 999U;
  BOOMPI_EXPECT(context, !invalid.valid());
  invalid = accepted;
  invalid.queued_sample_frames_before_write =
      PcmPlaybackWriteResult::kMaximumQueuedSampleFrames + 1U;
  BOOMPI_EXPECT(context, !invalid.valid());
  invalid = accepted;
  invalid.timing_source = PlaybackTimingSource::kUnset;
  BOOMPI_EXPECT(context, !invalid.valid());
  invalid.timing_source = static_cast<PlaybackTimingSource>(0xFFU);
  BOOMPI_EXPECT(context, !invalid.valid());
  invalid = accepted;
  invalid.native_error = -5;
  BOOMPI_EXPECT(context, !invalid.valid());

  constexpr std::array<PcmPlaybackWriteCode, 8U> kNonAcceptedCodes{{
      PcmPlaybackWriteCode::kWouldBlock,
      PcmPlaybackWriteCode::kInterrupted,
      PcmPlaybackWriteCode::kXrun,
      PcmPlaybackWriteCode::kSuspended,
      PcmPlaybackWriteCode::kDeviceLost,
      PcmPlaybackWriteCode::kZeroProgress,
      PcmPlaybackWriteCode::kFatal,
      PcmPlaybackWriteCode::kUnavailable,
  }};
  for (const PcmPlaybackWriteCode code : kNonAcceptedCodes) {
    const auto result = EmptyWriteResult(code);
    BOOMPI_EXPECT(context, result.valid());
    BOOMPI_EXPECT(context, !result.accepted());
  }
  const auto would_block_with_native_error =
      EmptyWriteResult(PcmPlaybackWriteCode::kWouldBlock, -11);
  BOOMPI_EXPECT(context, would_block_with_native_error.valid());
  BOOMPI_EXPECT(context, would_block_with_native_error.native_error == -11);
  BOOMPI_EXPECT(
      context,
      EmptyWriteResult(PcmPlaybackWriteCode::kWouldBlock)
          .retryable_without_reset());
  BOOMPI_EXPECT(
      context,
      EmptyWriteResult(PcmPlaybackWriteCode::kInterrupted)
          .retryable_without_reset());
  BOOMPI_EXPECT(
      context,
      !EmptyWriteResult(PcmPlaybackWriteCode::kZeroProgress)
           .retryable_without_reset());
  BOOMPI_EXPECT(context,
                EmptyWriteResult(PcmPlaybackWriteCode::kXrun)
                    .requires_stream_reset());
  BOOMPI_EXPECT(context,
                EmptyWriteResult(PcmPlaybackWriteCode::kUnavailable)
                    .requires_stream_reset());
  BOOMPI_EXPECT(context,
                EmptyWriteResult(PcmPlaybackWriteCode::kZeroProgress)
                    .requires_stream_reset());

  invalid = EmptyWriteResult(PcmPlaybackWriteCode::kWouldBlock);
  invalid.accepted_sample_frames = 1U;
  BOOMPI_EXPECT(context, !invalid.valid());
  invalid = EmptyWriteResult(PcmPlaybackWriteCode::kFatal);
  invalid.timing_source = PlaybackTimingSource::kAlsaStatus;
  BOOMPI_EXPECT(context, !invalid.valid());
  invalid = EmptyWriteResult(
      static_cast<PcmPlaybackWriteCode>(0xFFU));
  BOOMPI_EXPECT(context, !invalid.valid());

  constexpr std::array<PcmPlaybackControlCode, 5U> kControlCodes{{
      PcmPlaybackControlCode::kSucceeded,
      PcmPlaybackControlCode::kInterrupted,
      PcmPlaybackControlCode::kDeviceLost,
      PcmPlaybackControlCode::kFatal,
      PcmPlaybackControlCode::kUnavailable,
  }};
  for (const PcmPlaybackControlCode code : kControlCodes) {
    const PcmPlaybackControlResult result{code, 0};
    BOOMPI_EXPECT(context, result.valid());
    BOOMPI_EXPECT(
        context,
        result.succeeded() ==
            (code == PcmPlaybackControlCode::kSucceeded));
  }
  constexpr PcmPlaybackControlResult default_control{};
  static_assert(!default_control.valid() && !default_control.succeeded(),
                "value-initialized controls must fail closed");
  BOOMPI_EXPECT(context,
                default_control.code == PcmPlaybackControlCode::kUnset);
  BOOMPI_EXPECT(context, !default_control.valid());
  BOOMPI_EXPECT(context, !default_control.succeeded());
  const PcmPlaybackControlResult explicit_unset{
      PcmPlaybackControlCode::kUnset, -1};
  BOOMPI_EXPECT(context, !explicit_unset.valid());
  BOOMPI_EXPECT(context, !explicit_unset.succeeded());
  const PcmPlaybackControlResult invalid_control{
      static_cast<PcmPlaybackControlCode>(0xFFU), 0};
  BOOMPI_EXPECT(context, !invalid_control.valid());
  const PcmPlaybackControlResult success_with_error{
      PcmPlaybackControlCode::kSucceeded, -1};
  BOOMPI_EXPECT(context, !success_with_error.valid());
}

void TestUnavailableSink(boompi::test::TestContext& context) {
  UnavailablePcmPlaybackSink sink;
  const std::array<std::int16_t, 4U> samples{{1, 2, 3, 4}};
  auto result = sink.TryWriteInterleavedS16(
      samples.data(), static_cast<std::uint16_t>(samples.size()));
  BOOMPI_EXPECT(context, result.valid());
  BOOMPI_EXPECT(context, result.code == PcmPlaybackWriteCode::kUnavailable);
  BOOMPI_EXPECT(context, !result.accepted());
  result = sink.TryWriteInterleavedS16(nullptr, 0U);
  BOOMPI_EXPECT(context, result.valid());
  BOOMPI_EXPECT(context, result.code == PcmPlaybackWriteCode::kUnavailable);

  const auto drop = sink.Drop();
  const auto prepare = sink.Prepare();
  BOOMPI_EXPECT(context, drop.valid());
  BOOMPI_EXPECT(context, !drop.succeeded());
  BOOMPI_EXPECT(context,
                drop.code == PcmPlaybackControlCode::kUnavailable);
  BOOMPI_EXPECT(context, prepare.valid());
  BOOMPI_EXPECT(context, !prepare.succeeded());
  BOOMPI_EXPECT(context,
                prepare.code == PcmPlaybackControlCode::kUnavailable);
  BOOMPI_EXPECT(context, prepare.native_error == 0);
}

struct HookState final {
  ScriptedPcmPlaybackSink* sink;
  std::uint32_t stage;
  bool order_valid;
};

void PreWriteHook(void* const opaque) noexcept {
  auto* const state = static_cast<HookState*>(opaque);
  state->order_valid = state->order_valid && state->stage == 0U &&
                       state->sink->trace_size() == 0U;
  state->stage = 1U;
}

void PostWriteHook(void* const opaque) noexcept {
  auto* const state = static_cast<HookState*>(opaque);
  state->order_valid = state->order_valid && state->stage == 1U &&
                       state->sink->trace_size() == 1U;
  state->stage = 2U;
}

void TestScriptedWritesAndHooks(boompi::test::TestContext& context) {
  ScriptedPcmPlaybackSink sink;
  BOOMPI_EXPECT(
      context,
      sink.PushAccepted(3U, 1000U, 2500U, 72U,
                        PlaybackTimingSource::kAlsaStatus));
  BOOMPI_EXPECT(context,
                sink.PushWriteCode(PcmPlaybackWriteCode::kWouldBlock,
                                   -11));
  BOOMPI_EXPECT(context,
                sink.PushWriteCode(PcmPlaybackWriteCode::kInterrupted));
  BOOMPI_EXPECT(context,
                sink.PushWriteCode(PcmPlaybackWriteCode::kXrun));
  BOOMPI_EXPECT(context,
                sink.PushWriteCode(PcmPlaybackWriteCode::kSuspended));
  BOOMPI_EXPECT(context,
                sink.PushWriteCode(PcmPlaybackWriteCode::kDeviceLost));
  BOOMPI_EXPECT(context,
                sink.PushWriteCode(PcmPlaybackWriteCode::kZeroProgress));
  BOOMPI_EXPECT(context,
                sink.PushWriteCode(PcmPlaybackWriteCode::kFatal));
  BOOMPI_EXPECT(context,
                sink.PushWriteCode(PcmPlaybackWriteCode::kUnavailable));
  BOOMPI_EXPECT(context,
                !sink.PushWriteCode(PcmPlaybackWriteCode::kAccepted));
  BOOMPI_EXPECT(context,
                !sink.PushWriteResult(
                    {PcmPlaybackWriteCode::kAccepted, 1U, 0U, 0U, 0U,
                     PlaybackTimingSource::kUnset, 0}));
  BOOMPI_EXPECT(context, sink.pending_write_steps() == 9U);

  std::array<std::int16_t, 8U> samples{{11, 22, 33, 44, 55, 66, 77, 88}};
  HookState hook_state{&sink, 0U, true};
  sink.SetPreWriteHook(&PreWriteHook, &hook_state);
  sink.SetPostWriteHook(&PostWriteHook, &hook_state);
  auto result = sink.TryWriteInterleavedS16(samples.data() + 2U, 6U);
  BOOMPI_EXPECT(context, result.valid());
  BOOMPI_EXPECT(context, result.accepted());
  BOOMPI_EXPECT(context, result.accepted_sample_frames == 3U);
  BOOMPI_EXPECT(context, result.write_completed_timestamp_us == 1000U);
  BOOMPI_EXPECT(
      context, result.estimated_presentation_timestamp_us == 2500U);
  BOOMPI_EXPECT(context, result.queued_sample_frames_before_write == 72U);
  BOOMPI_EXPECT(context,
                result.timing_source == PlaybackTimingSource::kAlsaStatus);
  BOOMPI_EXPECT(context, hook_state.order_valid);
  BOOMPI_EXPECT(context, hook_state.stage == 2U);
  sink.ClearWriteHooks();

  const auto* trace = sink.trace_entry(0U);
  BOOMPI_EXPECT(context, trace != nullptr);
  if (trace != nullptr) {
    BOOMPI_EXPECT(context, trace->call == ScriptedPcmPlaybackCall::kWrite);
    BOOMPI_EXPECT(
        context,
        trace->sample_address ==
            reinterpret_cast<std::uintptr_t>(samples.data() + 2U));
    BOOMPI_EXPECT(context, trace->requested_frames == 6U);
    BOOMPI_EXPECT(context, trace->has_first_sample);
    BOOMPI_EXPECT(context, trace->first_sample == 33);
    BOOMPI_EXPECT(context,
                  trace->write_result.accepted_sample_frames == 3U);
  }

  constexpr std::array<PcmPlaybackWriteCode, 8U> kExpectedCodes{{
      PcmPlaybackWriteCode::kWouldBlock,
      PcmPlaybackWriteCode::kInterrupted,
      PcmPlaybackWriteCode::kXrun,
      PcmPlaybackWriteCode::kSuspended,
      PcmPlaybackWriteCode::kDeviceLost,
      PcmPlaybackWriteCode::kZeroProgress,
      PcmPlaybackWriteCode::kFatal,
      PcmPlaybackWriteCode::kUnavailable,
  }};
  for (const PcmPlaybackWriteCode code : kExpectedCodes) {
    result = sink.TryWriteInterleavedS16(samples.data(), 4U);
    BOOMPI_EXPECT(context, result.valid());
    BOOMPI_EXPECT(context, result.code == code);
    BOOMPI_EXPECT(
        context,
        result.native_error ==
            (code == PcmPlaybackWriteCode::kWouldBlock ? -11 : 0));
  }
  BOOMPI_EXPECT(context, sink.pending_write_steps() == 0U);
  result = sink.TryWriteInterleavedS16(samples.data(), 4U);
  BOOMPI_EXPECT(context, result.code == PcmPlaybackWriteCode::kUnavailable);
  BOOMPI_EXPECT(context, sink.write_call_count() == 10U);
  BOOMPI_EXPECT(context, !sink.trace_overflowed());
  BOOMPI_EXPECT(context, sink.trace_entry(sink.trace_size()) == nullptr);
}

void TestInvalidArgumentsDoNotConsumeScript(
    boompi::test::TestContext& context) {
  ScriptedPcmPlaybackSink sink;
  BOOMPI_EXPECT(
      context,
      sink.PushAccepted(1U, 5000U, 6000U, 0U,
                        PlaybackTimingSource::kSoftwareEstimate));
  auto result = sink.TryWriteInterleavedS16(nullptr, 1U);
  BOOMPI_EXPECT(context, result.code == PcmPlaybackWriteCode::kFatal);
  BOOMPI_EXPECT(context, result.valid());
  BOOMPI_EXPECT(context, sink.pending_write_steps() == 1U);
  std::int16_t sample = 123;
  result = sink.TryWriteInterleavedS16(&sample, 0U);
  BOOMPI_EXPECT(context, result.code == PcmPlaybackWriteCode::kFatal);
  BOOMPI_EXPECT(context, sink.pending_write_steps() == 1U);
  result = sink.TryWriteInterleavedS16(&sample, 1U);
  BOOMPI_EXPECT(context, result.accepted());
  BOOMPI_EXPECT(context, sink.pending_write_steps() == 0U);
}

void TestUncheckedProtocolInjection(
    boompi::test::TestContext& context) {
  ScriptedPcmPlaybackSink sink;
  const PcmPlaybackWriteResult malformed_accepted{
      PcmPlaybackWriteCode::kAccepted, 7U, 0U, 0U, 0U,
      PlaybackTimingSource::kUnset, 0};
  BOOMPI_EXPECT(context, !malformed_accepted.valid());
  BOOMPI_EXPECT(context, !sink.PushWriteResult(malformed_accepted));
  BOOMPI_EXPECT(
      context,
      sink.PushUncheckedWriteResultForProtocolTest(malformed_accepted));

  std::array<std::int16_t, 8U> samples{{1, 2, 3, 4, 5, 6, 7, 8}};
  const auto write = sink.TryWriteInterleavedS16(samples.data(), 8U);
  BOOMPI_EXPECT(context, write.code == PcmPlaybackWriteCode::kAccepted);
  BOOMPI_EXPECT(context, write.accepted_sample_frames == 7U);
  BOOMPI_EXPECT(context, !write.valid());
  BOOMPI_EXPECT(context, sink.pending_write_steps() == 0U);
  BOOMPI_EXPECT(context, sink.trace_entry(0U) != nullptr);
  if (sink.trace_entry(0U) != nullptr) {
    BOOMPI_EXPECT(context, !sink.trace_entry(0U)->write_result.valid());
  }

  const PcmPlaybackControlResult malformed_drop{
      PcmPlaybackControlCode::kUnset, 0};
  const PcmPlaybackControlResult malformed_prepare{
      PcmPlaybackControlCode::kSucceeded, -5};
  BOOMPI_EXPECT(context, !sink.PushDropResult(malformed_drop));
  BOOMPI_EXPECT(context, !sink.PushPrepareResult(malformed_prepare));
  BOOMPI_EXPECT(
      context,
      sink.PushUncheckedDropResultForProtocolTest(malformed_drop));
  BOOMPI_EXPECT(
      context,
      sink.PushUncheckedPrepareResultForProtocolTest(malformed_prepare));

  const auto drop = sink.Drop();
  const auto prepare = sink.Prepare();
  BOOMPI_EXPECT(context, drop.code == PcmPlaybackControlCode::kUnset);
  BOOMPI_EXPECT(context, !drop.valid());
  BOOMPI_EXPECT(context, !drop.succeeded());
  BOOMPI_EXPECT(context,
                prepare.code == PcmPlaybackControlCode::kSucceeded);
  BOOMPI_EXPECT(context, !prepare.valid());
  BOOMPI_EXPECT(context, !prepare.succeeded());
}

void TestControlScriptsAndOrdering(boompi::test::TestContext& context) {
  ScriptedPcmPlaybackSink sink;
  BOOMPI_EXPECT(
      context,
      sink.PushDropResult({PcmPlaybackControlCode::kInterrupted, -4}));
  BOOMPI_EXPECT(
      context,
      sink.PushDropResult({PcmPlaybackControlCode::kDeviceLost, -19}));
  BOOMPI_EXPECT(
      context,
      sink.PushPrepareResult({PcmPlaybackControlCode::kFatal, -5}));
  BOOMPI_EXPECT(
      context,
      sink.PushPrepareResult({PcmPlaybackControlCode::kUnavailable, 0}));
  BOOMPI_EXPECT(
      context,
      !sink.PushDropResult(
          {static_cast<PcmPlaybackControlCode>(0xFFU), 0}));

  auto control = sink.Drop();
  BOOMPI_EXPECT(context,
                control.code == PcmPlaybackControlCode::kInterrupted);
  control = sink.Prepare();
  BOOMPI_EXPECT(context, control.code == PcmPlaybackControlCode::kFatal);
  control = sink.Drop();
  BOOMPI_EXPECT(context,
                control.code == PcmPlaybackControlCode::kDeviceLost);
  control = sink.Prepare();
  BOOMPI_EXPECT(context,
                control.code == PcmPlaybackControlCode::kUnavailable);
  BOOMPI_EXPECT(context, sink.pending_drop_steps() == 0U);
  BOOMPI_EXPECT(context, sink.pending_prepare_steps() == 0U);
  const auto unscripted_drop = sink.Drop();
  const auto unscripted_prepare = sink.Prepare();
  BOOMPI_EXPECT(
      context,
      unscripted_drop.code == PcmPlaybackControlCode::kUnavailable);
  BOOMPI_EXPECT(context, !unscripted_drop.succeeded());
  BOOMPI_EXPECT(
      context,
      unscripted_prepare.code == PcmPlaybackControlCode::kUnavailable);
  BOOMPI_EXPECT(context, !unscripted_prepare.succeeded());
  BOOMPI_EXPECT(context, sink.drop_call_count() == 3U);
  BOOMPI_EXPECT(context, sink.prepare_call_count() == 3U);

  constexpr std::array<ScriptedPcmPlaybackCall, 6U> kExpectedCalls{{
      ScriptedPcmPlaybackCall::kDrop,
      ScriptedPcmPlaybackCall::kPrepare,
      ScriptedPcmPlaybackCall::kDrop,
      ScriptedPcmPlaybackCall::kPrepare,
      ScriptedPcmPlaybackCall::kDrop,
      ScriptedPcmPlaybackCall::kPrepare,
  }};
  BOOMPI_EXPECT(context, sink.trace_size() == kExpectedCalls.size());
  for (std::size_t index = 0U; index < kExpectedCalls.size(); ++index) {
    const auto* const trace = sink.trace_entry(index);
    BOOMPI_EXPECT(context, trace != nullptr);
    if (trace != nullptr) {
      BOOMPI_EXPECT(context, trace->call == kExpectedCalls[index]);
      BOOMPI_EXPECT(context, trace->sample_address == 0U);
      BOOMPI_EXPECT(context, !trace->has_first_sample);
      BOOMPI_EXPECT(context, trace->control_result.valid());
    }
  }
  const auto* const first_drop = sink.trace_entry(0U);
  const auto* const first_prepare = sink.trace_entry(1U);
  BOOMPI_EXPECT(context, first_drop != nullptr);
  BOOMPI_EXPECT(context, first_prepare != nullptr);
  if (first_drop != nullptr && first_prepare != nullptr) {
    BOOMPI_EXPECT(context, first_drop->control_result.native_error == -4);
    BOOMPI_EXPECT(context,
                  first_prepare->control_result.native_error == -5);
  }
}

void TestFixedCapacitiesAndReset(boompi::test::TestContext& context) {
  ScriptedPcmPlaybackSink sink;
  for (std::size_t step = 0U;
       step < ScriptedPcmPlaybackSink::kMaximumWriteSteps; ++step) {
    BOOMPI_EXPECT(
        context,
        sink.PushWriteCode(PcmPlaybackWriteCode::kWouldBlock));
  }
  BOOMPI_EXPECT(
      context,
      !sink.PushWriteCode(PcmPlaybackWriteCode::kWouldBlock));

  for (std::size_t step = 0U;
       step < ScriptedPcmPlaybackSink::kMaximumControlSteps; ++step) {
    BOOMPI_EXPECT(
        context,
        sink.PushDropResult({PcmPlaybackControlCode::kSucceeded, 0}));
    BOOMPI_EXPECT(
        context,
        sink.PushPrepareResult({PcmPlaybackControlCode::kSucceeded, 0}));
  }
  BOOMPI_EXPECT(
      context,
      !sink.PushDropResult({PcmPlaybackControlCode::kSucceeded, 0}));
  BOOMPI_EXPECT(
      context,
      !sink.PushPrepareResult({PcmPlaybackControlCode::kSucceeded, 0}));

  sink.ClearScripts();
  BOOMPI_EXPECT(context, sink.pending_write_steps() == 0U);
  BOOMPI_EXPECT(context, sink.pending_drop_steps() == 0U);
  BOOMPI_EXPECT(context, sink.pending_prepare_steps() == 0U);
  std::int16_t sample = 5;
  BOOMPI_EXPECT(
      context,
      sink.TryWriteInterleavedS16(&sample, 1U).code ==
          PcmPlaybackWriteCode::kUnavailable);
  BOOMPI_EXPECT(context, sink.trace_size() == 1U);
  sink.ResetTrace();
  BOOMPI_EXPECT(context, sink.trace_size() == 0U);
  BOOMPI_EXPECT(context, sink.write_call_count() == 0U);
  BOOMPI_EXPECT(context, !sink.trace_overflowed());

  for (std::size_t call = 0U;
       call <= ScriptedPcmPlaybackSink::kMaximumTraceEntries; ++call) {
    static_cast<void>(sink.TryWriteInterleavedS16(&sample, 1U));
  }
  BOOMPI_EXPECT(
      context,
      sink.trace_size() == ScriptedPcmPlaybackSink::kMaximumTraceEntries);
  BOOMPI_EXPECT(context, sink.trace_overflowed());
  BOOMPI_EXPECT(
      context,
      sink.write_call_count() ==
          ScriptedPcmPlaybackSink::kMaximumTraceEntries + 1U);
}

}  // namespace

int main() {
  boompi::test::TestContext context;
  TestWriteResultContract(context);
  TestUnavailableSink(context);
  TestScriptedWritesAndHooks(context);
  TestInvalidArgumentsDoNotConsumeScript(context);
  TestUncheckedProtocolInjection(context);
  TestControlScriptsAndOrdering(context);
  TestFixedCapacitiesAndReset(context);

  if (context.failures() != 0U) {
    std::cerr << context.failures()
              << " PCM playback sink expectation(s) failed\n";
    return 1;
  }
  std::cout << "boomPI PCM playback sink tests passed\n";
  return 0;
}
