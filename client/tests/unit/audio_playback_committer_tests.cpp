#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <type_traits>
#include <utility>

#include "boompi/audio/playback_committer.h"
#include "boompi/test/scripted_pcm_playback_sink.h"
#include "boompi/test/test_context.h"

#define BOOMPI_EXPECT(context, expression) \
  (context).Expect((expression), #expression, __FILE__, __LINE__)

namespace {

using boompi::audio::AcceptedRenderChunk48k;
using boompi::audio::AcceptedRenderQueue;
using boompi::audio::PcmPlaybackControlCode;
using boompi::audio::PcmPlaybackWriteCode;
using boompi::audio::PlaybackCancelCode;
using boompi::audio::PlaybackCancelResult;
using boompi::audio::PlaybackCommitCode;
using boompi::audio::PlaybackCommitResult;
using boompi::audio::PlaybackCommitter;
using boompi::audio::PlaybackCommitterConfig;
using boompi::audio::PlaybackCommitterState;
using boompi::audio::PlaybackEpochFence;
using boompi::audio::PlaybackGeneration;
using boompi::audio::PlaybackPcmFrame48k;
using boompi::audio::PlaybackReferencePolicy;
using boompi::audio::PlaybackReferenceResetCode;
using boompi::audio::PlaybackReferenceResetResult;
using boompi::audio::PlaybackSubmitCode;
using boompi::audio::PlaybackSubmitResult;
using boompi::audio::PlaybackTimingSource;
using boompi::test::ScriptedPcmPlaybackCall;
using boompi::test::ScriptedPcmPlaybackSink;

static_assert(std::is_trivial<PlaybackCommitResult>::value,
              "committer result must remain a POD value");
static_assert(std::is_standard_layout<PlaybackCommitResult>::value,
              "committer result must remain standard-layout");
static_assert(PlaybackSubmitResult{}.code == PlaybackSubmitCode::kUnset &&
                  !PlaybackSubmitResult{}.accepted(),
              "default playback submit result must fail closed");
static_assert(PlaybackCommitResult{}.code == PlaybackCommitCode::kUnset &&
                  !PlaybackCommitResult{}.made_progress(),
              "default playback commit result must fail closed");
static_assert(PlaybackCancelResult{}.code == PlaybackCancelCode::kUnset &&
                  !PlaybackCancelResult{}.acknowledged,
              "default playback cancel result must fail closed");
static_assert(
    PlaybackReferenceResetResult{}.code ==
            PlaybackReferenceResetCode::kUnset &&
        !PlaybackReferenceResetResult{}.confirmed,
    "default playback reference reset result must fail closed");
static_assert(
    noexcept(std::declval<PlaybackCommitter&>().Submit(
        std::declval<const PlaybackPcmFrame48k&>())),
    "committer Submit must remain noexcept");
static_assert(noexcept(std::declval<PlaybackCommitter&>().PumpOnce()),
              "committer PumpOnce must remain noexcept");
static_assert(
    noexcept(std::declval<PlaybackCommitter&>().Cancel(
        1U, std::declval<const PlaybackGeneration&>())),
    "committer Cancel must remain noexcept");
static_assert(
    noexcept(std::declval<PlaybackCommitter&>().ConfirmReferenceReset(
        1U, std::declval<const PlaybackGeneration&>())),
    "committer reference-reset confirmation must remain noexcept");

PlaybackGeneration Generation(const std::uint32_t epoch,
                              const std::uint32_t turn_id,
                              const std::uint32_t stream_id) {
  return {epoch, turn_id, stream_id};
}

PlaybackPcmFrame48k MakeFrame(
    const PlaybackGeneration& generation, const std::uint32_t sequence,
    const std::uint64_t timestamp_us, const std::int16_t sample_base = 1000,
    const std::uint16_t valid_samples =
        PlaybackPcmFrame48k::kFrameSamples,
    const std::uint16_t source_offset = 0U,
    const bool end_of_stream = false) {
  PlaybackPcmFrame48k frame{};
  frame.format = {PlaybackPcmFrame48k::kSampleRateHz,
                  PlaybackPcmFrame48k::kFrameDurationMs,
                  PlaybackPcmFrame48k::kChannelCount,
                  PlaybackPcmFrame48k::kSampleFormat};
  frame.metadata.monotonic_timestamp_us = timestamp_us;
  frame.metadata.sequence = sequence;
  frame.metadata.epoch = generation.epoch;
  frame.metadata.turn_id = generation.turn_id;
  frame.metadata.stream_id = generation.stream_id;
  frame.source_offset_sample_frames = source_offset;
  frame.valid_samples = valid_samples;
  frame.end_of_stream = end_of_stream;
  for (std::size_t sample = 0U; sample < valid_samples; ++sample) {
    frame.samples[sample] = static_cast<std::int16_t>(
        static_cast<std::int32_t>(sample_base) +
        static_cast<std::int32_t>(sample));
  }
  return frame;
}

void ScriptSuccessfulCancel(boompi::test::TestContext& context,
                            ScriptedPcmPlaybackSink* const sink) {
  BOOMPI_EXPECT(
      context,
      sink->PushDropResult({PcmPlaybackControlCode::kSucceeded, 0}));
  BOOMPI_EXPECT(
      context,
      sink->PushPrepareResult({PcmPlaybackControlCode::kSucceeded, 0}));
}

bool ConfigureAndArm(boompi::test::TestContext& context,
                     const PlaybackCommitterConfig& config,
                     PlaybackEpochFence* const fence,
                     ScriptedPcmPlaybackSink* const sink,
                     AcceptedRenderQueue* const queue,
                     const PlaybackGeneration& generation,
                     PlaybackCommitter* const committer) {
  const auto create_status = PlaybackCommitter::Create(
      config, fence, sink, queue, committer);
  BOOMPI_EXPECT(context, create_status.ok());
  if (!create_status.ok()) {
    return false;
  }
  BOOMPI_EXPECT(
      context,
      sink->PushPrepareResult({PcmPlaybackControlCode::kSucceeded, 0}));
  const auto initialize_status = committer->Initialize();
  BOOMPI_EXPECT(context, initialize_status.ok());
  if (!initialize_status.ok()) {
    return false;
  }
  BOOMPI_EXPECT(context, fence->AdvanceTo(generation.epoch).ok());
  const auto arm_status = committer->Arm(generation);
  BOOMPI_EXPECT(context, arm_status.ok());
  return arm_status.ok();
}

bool PopAccepted(AcceptedRenderQueue* const queue,
                 AcceptedRenderChunk48k* const output) {
  auto read = queue->TryAcquireRead();
  if (!read) {
    return false;
  }
  *output = read.frame();
  return read.Release();
}

void ExpectAcceptedSlice(boompi::test::TestContext& context,
                         const AcceptedRenderChunk48k& chunk,
                         const PlaybackPcmFrame48k& source,
                         const std::uint16_t cursor,
                         const std::uint16_t accepted,
                         const std::uint64_t sequence,
                         const bool completes,
                         const bool source_end_of_stream) {
  BOOMPI_EXPECT(context, chunk.HasValidLength());
  BOOMPI_EXPECT(context, chunk.metadata.sequence == source.metadata.sequence);
  BOOMPI_EXPECT(context, chunk.metadata.epoch == source.metadata.epoch);
  BOOMPI_EXPECT(context, chunk.metadata.turn_id == source.metadata.turn_id);
  BOOMPI_EXPECT(context, chunk.metadata.stream_id == source.metadata.stream_id);
  BOOMPI_EXPECT(context, chunk.accepted_chunk_sequence == sequence);
  BOOMPI_EXPECT(
      context,
      chunk.absolute_source_offset_sample_frames ==
          source.source_offset_sample_frames + cursor);
  BOOMPI_EXPECT(context, chunk.accepted_samples == accepted);
  BOOMPI_EXPECT(context, chunk.completes_render_chunk == completes);
  BOOMPI_EXPECT(context,
                chunk.source_end_of_stream == source_end_of_stream);
  bool samples_match = true;
  for (std::size_t sample = 0U; sample < accepted; ++sample) {
    samples_match =
        samples_match && chunk.samples[sample] == source.samples[cursor + sample];
  }
  BOOMPI_EXPECT(context, samples_match);
  bool tail_is_zero = true;
  for (std::size_t sample = accepted; sample < chunk.samples.size(); ++sample) {
    tail_is_zero = tail_is_zero && chunk.samples[sample] == 0;
  }
  BOOMPI_EXPECT(context, tail_is_zero);
}

void FillAcceptedQueue(AcceptedRenderQueue* const queue) {
  for (std::uint32_t index = 0U;
       index < boompi::audio::kAcceptedRenderQueueCapacity; ++index) {
    auto write = queue->TryAcquireWrite();
    if (!write) {
      return;
    }
    auto& chunk = write.frame();
    chunk.format = {AcceptedRenderChunk48k::kSampleRateHz,
                    AcceptedRenderChunk48k::kFrameDurationMs,
                    AcceptedRenderChunk48k::kChannelCount,
                    AcceptedRenderChunk48k::kSampleFormat};
    chunk.metadata.monotonic_timestamp_us = 1000000U + index;
    chunk.metadata.sequence = index;
    chunk.metadata.epoch = 100U;
    chunk.metadata.turn_id = 101U;
    chunk.metadata.stream_id = 102U;
    chunk.pcm_incarnation = 1U;
    chunk.accepted_chunk_sequence = index + 1U;
    chunk.absolute_source_offset_sample_frames = 0U;
    chunk.accepted_samples = 1U;
    chunk.completes_render_chunk = true;
    chunk.write_completed_timestamp_us = 2000000U + index;
    chunk.estimated_presentation_timestamp_us = 3000000U + index;
    chunk.timing_source = PlaybackTimingSource::kSoftwareEstimate;
    chunk.samples[0U] = static_cast<std::int16_t>(index);
    if (!write.Publish()) {
      return;
    }
  }
}

void TestConfigurationAndInitialization(
    boompi::test::TestContext& context) {
  PlaybackCommitter committer;
  PlaybackEpochFence fence;
  ScriptedPcmPlaybackSink sink;
  AcceptedRenderQueue queue;
  PlaybackCommitterConfig config{};

  {
    PlaybackCommitter fail_closed_committer;
    PlaybackEpochFence fail_closed_fence;
    ScriptedPcmPlaybackSink fail_closed_sink;
    AcceptedRenderQueue fail_closed_queue;
    BOOMPI_EXPECT(
        context,
        fail_closed_sink.PushUncheckedPrepareResultForProtocolTest({}));
    BOOMPI_EXPECT(
        context,
        PlaybackCommitter::Create(config, &fail_closed_fence,
                                  &fail_closed_sink, &fail_closed_queue,
                                  &fail_closed_committer)
            .ok());
    BOOMPI_EXPECT(context, !fail_closed_committer.Initialize().ok());
    BOOMPI_EXPECT(
        context,
        fail_closed_committer.state() == PlaybackCommitterState::kSinkFault);
  }

  BOOMPI_EXPECT(
      context,
      !PlaybackCommitter::Create(config, nullptr, &sink, &queue, &committer)
           .ok());
  BOOMPI_EXPECT(
      context,
      !PlaybackCommitter::Create(config, &fence, nullptr, &queue, &committer)
           .ok());
  BOOMPI_EXPECT(
      context,
      !PlaybackCommitter::Create(config, &fence, &sink, nullptr, &committer)
           .ok());
  auto invalid = config;
  invalid.reference_policy = static_cast<PlaybackReferencePolicy>(99U);
  BOOMPI_EXPECT(
      context,
      !PlaybackCommitter::Create(invalid, &fence, &sink, &queue, &committer)
           .ok());
  invalid = config;
  invalid.initial_pcm_incarnation = 0U;
  BOOMPI_EXPECT(
      context,
      !PlaybackCommitter::Create(invalid, &fence, &sink, &queue, &committer)
           .ok());
  BOOMPI_EXPECT(context,
                PlaybackCommitter::Create(config, &fence, &sink, &queue,
                                           &committer)
                    .ok());
  BOOMPI_EXPECT(context,
                committer.state() == PlaybackCommitterState::kUnprepared);
  BOOMPI_EXPECT(
      context,
      sink.PushPrepareResult({PcmPlaybackControlCode::kSucceeded, 0}));
  BOOMPI_EXPECT(context, committer.Initialize().ok());
  BOOMPI_EXPECT(context, committer.pcm_incarnation() == 1U);
  BOOMPI_EXPECT(context,
                committer.state() == PlaybackCommitterState::kPreparedIdle);
  BOOMPI_EXPECT(context, !committer.Initialize().ok());
  BOOMPI_EXPECT(context,
                !PlaybackCommitter::Create(config, &fence, &sink, &queue,
                                           &committer)
                     .ok());
}

void TestPartialWritesAreExactlyOnce(
    boompi::test::TestContext& context) {
  const auto generation = Generation(1U, 10U, 100U);
  PlaybackEpochFence fence;
  ScriptedPcmPlaybackSink sink;
  AcceptedRenderQueue queue;
  PlaybackCommitter committer;
  if (!ConfigureAndArm(context, PlaybackCommitterConfig{}, &fence, &sink,
                       &queue, generation, &committer)) {
    return;
  }
  sink.ResetTrace();
  BOOMPI_EXPECT(context,
                sink.PushAccepted(200U, 1000U, 2000U, 0U,
                                  PlaybackTimingSource::kSoftwareEstimate));
  BOOMPI_EXPECT(
      context,
      sink.PushWriteCode(PcmPlaybackWriteCode::kWouldBlock, -11));
  BOOMPI_EXPECT(context,
                sink.PushAccepted(300U, 1100U, 2100U, 200U,
                                  PlaybackTimingSource::kAlsaStatus));
  BOOMPI_EXPECT(
      context,
      sink.PushWriteCode(PcmPlaybackWriteCode::kInterrupted, -4));
  BOOMPI_EXPECT(context,
                sink.PushAccepted(460U, 1200U, 2200U, 500U,
                                  PlaybackTimingSource::kSoftwareEstimate));

  const auto frame = MakeFrame(generation, 0U, 5000000U, 2000);
  BOOMPI_EXPECT(context, frame.HasValidLength());
  BOOMPI_EXPECT(context, committer.Submit(frame).accepted());

  auto result = committer.PumpOnce();
  BOOMPI_EXPECT(context, result.code == PlaybackCommitCode::kAcceptedPartial);
  BOOMPI_EXPECT(context, result.accepted_sample_frames == 200U);
  BOOMPI_EXPECT(context, result.remaining_in_current_chunk == 760U);
  AcceptedRenderChunk48k chunk{};
  BOOMPI_EXPECT(context, PopAccepted(&queue, &chunk));
  ExpectAcceptedSlice(context, chunk, frame, 0U, 200U, 1U, false, false);

  result = committer.PumpOnce();
  BOOMPI_EXPECT(context, result.code == PlaybackCommitCode::kWouldBlock);
  BOOMPI_EXPECT(context, result.accepted_from_current_chunk == 200U);
  BOOMPI_EXPECT(context, !queue.TryAcquireRead());

  result = committer.PumpOnce();
  BOOMPI_EXPECT(context, result.code == PlaybackCommitCode::kAcceptedPartial);
  BOOMPI_EXPECT(context, result.accepted_sample_frames == 300U);
  BOOMPI_EXPECT(context, PopAccepted(&queue, &chunk));
  ExpectAcceptedSlice(context, chunk, frame, 200U, 300U, 2U, false, false);

  result = committer.PumpOnce();
  BOOMPI_EXPECT(context, result.code == PlaybackCommitCode::kInterrupted);
  BOOMPI_EXPECT(context, result.accepted_from_current_chunk == 500U);

  result = committer.PumpOnce();
  BOOMPI_EXPECT(context, result.code == PlaybackCommitCode::kChunkAccepted);
  BOOMPI_EXPECT(context, result.chunk_completed);
  BOOMPI_EXPECT(context, result.generation_accepted_sample_frames == 960U);
  BOOMPI_EXPECT(context, PopAccepted(&queue, &chunk));
  ExpectAcceptedSlice(context, chunk, frame, 500U, 460U, 3U, true, false);
  BOOMPI_EXPECT(context,
                committer.state() == PlaybackCommitterState::kArmedIdle);

  constexpr std::array<std::uint16_t, 5U> kRequested{{
      960U, 760U, 760U, 460U, 460U}};
  constexpr std::array<std::int16_t, 5U> kFirstSample{{
      2000, 2200, 2200, 2500, 2500}};
  BOOMPI_EXPECT(context, sink.trace_size() == kRequested.size());
  for (std::size_t index = 0U; index < kRequested.size(); ++index) {
    const auto* trace = sink.trace_entry(index);
    BOOMPI_EXPECT(context, trace != nullptr);
    if (trace != nullptr) {
      BOOMPI_EXPECT(context, trace->call == ScriptedPcmPlaybackCall::kWrite);
      BOOMPI_EXPECT(context, trace->requested_frames == kRequested[index]);
      BOOMPI_EXPECT(context, trace->first_sample == kFirstSample[index]);
    }
  }

  BOOMPI_EXPECT(context, fence.AdvanceTo(2U).ok());
  ScriptSuccessfulCancel(context, &sink);
  BOOMPI_EXPECT(context, committer.Cancel(1U, generation).acknowledged);
}

void TestReferenceBackpressurePreventsWrite(
    boompi::test::TestContext& context) {
  AcceptedRenderQueue queue;
  FillAcceptedQueue(&queue);
  BOOMPI_EXPECT(
      context,
      queue.size_approx() == boompi::audio::kAcceptedRenderQueueCapacity);

  const auto generation = Generation(10U, 11U, 12U);
  PlaybackEpochFence fence;
  ScriptedPcmPlaybackSink sink;
  PlaybackCommitter committer;
  if (!ConfigureAndArm(context, PlaybackCommitterConfig{}, &fence, &sink,
                       &queue, generation, &committer)) {
    return;
  }
  sink.ResetTrace();
  BOOMPI_EXPECT(context,
                sink.PushAccepted(960U, 1000U, 2000U, 0U,
                                  PlaybackTimingSource::kSoftwareEstimate));
  BOOMPI_EXPECT(context,
                committer.Submit(MakeFrame(generation, 0U, 6000000U))
                    .accepted());
  const auto result = committer.PumpOnce();
  BOOMPI_EXPECT(
      context, result.code == PlaybackCommitCode::kReferenceBackpressure);
  BOOMPI_EXPECT(context, sink.write_call_count() == 0U);
  BOOMPI_EXPECT(context, sink.pending_write_steps() == 1U);
  BOOMPI_EXPECT(context, fence.AdvanceTo(11U).ok());
  ScriptSuccessfulCancel(context, &sink);
  BOOMPI_EXPECT(context, committer.Cancel(1U, generation).acknowledged);
}

void TestCancelBarrierAndLateCancel(
    boompi::test::TestContext& context) {
  const auto generation = Generation(20U, 21U, 22U);
  PlaybackEpochFence fence;
  ScriptedPcmPlaybackSink sink;
  AcceptedRenderQueue queue;
  PlaybackCommitter committer;
  if (!ConfigureAndArm(context, PlaybackCommitterConfig{}, &fence, &sink,
                       &queue, generation, &committer)) {
    return;
  }
  sink.ResetTrace();
  BOOMPI_EXPECT(context,
                committer.Submit(MakeFrame(generation, 0U, 7000000U))
                    .accepted());
  auto early = committer.Cancel(1U, generation);
  BOOMPI_EXPECT(context, early.code == PlaybackCancelCode::kFenceNotAdvanced);
  BOOMPI_EXPECT(context, !early.acknowledged);
  BOOMPI_EXPECT(context, sink.drop_call_count() == 0U);

  BOOMPI_EXPECT(context, fence.AdvanceTo(21U).ok());
  const auto before_write = committer.PumpOnce();
  BOOMPI_EXPECT(
      context,
      before_write.code ==
          PlaybackCommitCode::kCancellationRequiredBeforeWrite);
  BOOMPI_EXPECT(context, sink.write_call_count() == 0U);

  const auto wrong = committer.Cancel(2U, Generation(20U, 99U, 22U));
  BOOMPI_EXPECT(context,
                wrong.code == PlaybackCancelCode::kGenerationMismatch);
  BOOMPI_EXPECT(context, sink.drop_call_count() == 0U);

  ScriptSuccessfulCancel(context, &sink);
  const auto acknowledged = committer.Cancel(3U, generation);
  BOOMPI_EXPECT(context, acknowledged.acknowledged);
  BOOMPI_EXPECT(context,
                acknowledged.code == PlaybackCancelCode::kAcknowledged);
  BOOMPI_EXPECT(context, acknowledged.drop_succeeded);
  BOOMPI_EXPECT(context, acknowledged.prepare_succeeded);
  BOOMPI_EXPECT(context, acknowledged.reference_reset_required);
  BOOMPI_EXPECT(context, acknowledged.discarded_pending_sample_frames == 960U);
  BOOMPI_EXPECT(context, acknowledged.retired_pcm_incarnation == 1U);
  BOOMPI_EXPECT(context, acknowledged.prepared_pcm_incarnation == 2U);
  BOOMPI_EXPECT(
      context,
      committer.state() == PlaybackCommitterState::kAwaitingReferenceReset);
  BOOMPI_EXPECT(context, sink.drop_call_count() == 1U);
  BOOMPI_EXPECT(context, sink.prepare_call_count() == 1U);
  BOOMPI_EXPECT(context, sink.trace_size() == 2U);
  BOOMPI_EXPECT(context,
                sink.trace_entry(0U)->call == ScriptedPcmPlaybackCall::kDrop);
  BOOMPI_EXPECT(
      context,
      sink.trace_entry(1U)->call == ScriptedPcmPlaybackCall::kPrepare);

  const auto duplicate = committer.Cancel(3U, generation);
  BOOMPI_EXPECT(
      context,
      duplicate.code == PlaybackCancelCode::kDuplicateAcknowledged);
  BOOMPI_EXPECT(context, duplicate.acknowledged);
  BOOMPI_EXPECT(context, sink.drop_call_count() == 1U);
  BOOMPI_EXPECT(context, sink.prepare_call_count() == 1U);

  const auto next_generation = Generation(21U, 23U, 24U);
  BOOMPI_EXPECT(context, !committer.Arm(next_generation).ok());
  BOOMPI_EXPECT(
      context,
      committer.ConfirmReferenceReset(99U, generation).code ==
          PlaybackReferenceResetCode::kRequestMismatch);
  BOOMPI_EXPECT(
      context,
      committer.ConfirmReferenceReset(3U, Generation(20U, 99U, 22U)).code ==
          PlaybackReferenceResetCode::kGenerationMismatch);
  const auto reset = committer.ConfirmReferenceReset(3U, generation);
  BOOMPI_EXPECT(context, reset.code == PlaybackReferenceResetCode::kConfirmed);
  BOOMPI_EXPECT(context, reset.confirmed);
  BOOMPI_EXPECT(
      context,
      committer.ConfirmReferenceReset(3U, generation).code ==
          PlaybackReferenceResetCode::kDuplicateConfirmed);
  BOOMPI_EXPECT(context, committer.Arm(next_generation).ok());
  const auto late = committer.Cancel(4U, generation);
  BOOMPI_EXPECT(context,
                late.code == PlaybackCancelCode::kGenerationMismatch);
  BOOMPI_EXPECT(context, sink.drop_call_count() == 1U);
}

struct AdvanceFenceHook final {
  PlaybackEpochFence* fence;
  std::uint32_t epoch;
  bool succeeded;
};

void AdvanceFence(void* const opaque) noexcept {
  auto* const hook = static_cast<AdvanceFenceHook*>(opaque);
  hook->succeeded = hook->fence->AdvanceTo(hook->epoch).ok();
}

void TestPositiveWriteRacingCancelIsRecorded(
    boompi::test::TestContext& context) {
  const auto generation = Generation(30U, 31U, 32U);
  PlaybackEpochFence fence;
  ScriptedPcmPlaybackSink sink;
  AcceptedRenderQueue queue;
  PlaybackCommitter committer;
  if (!ConfigureAndArm(context, PlaybackCommitterConfig{}, &fence, &sink,
                       &queue, generation, &committer)) {
    return;
  }
  const auto frame = MakeFrame(generation, 0U, 8000000U, 3000);
  BOOMPI_EXPECT(context, committer.Submit(frame).accepted());
  BOOMPI_EXPECT(context,
                sink.PushAccepted(200U, 3000U, 4000U, 100U,
                                  PlaybackTimingSource::kAlsaStatus));
  AdvanceFenceHook hook{&fence, 31U, false};
  sink.SetPreWriteHook(&AdvanceFence, &hook);

  const auto result = committer.PumpOnce();
  BOOMPI_EXPECT(context, hook.succeeded);
  BOOMPI_EXPECT(
      context,
      result.code == PlaybackCommitCode::kAcceptedThenCancellationRequired);
  BOOMPI_EXPECT(context, result.accepted_sample_frames == 200U);
  BOOMPI_EXPECT(context, result.remaining_in_current_chunk == 760U);
  BOOMPI_EXPECT(context, result.cancellation_observed_after_accept);
  BOOMPI_EXPECT(context,
                committer.state() ==
                    PlaybackCommitterState::kCancellationRequired);
  AcceptedRenderChunk48k chunk{};
  BOOMPI_EXPECT(context, PopAccepted(&queue, &chunk));
  ExpectAcceptedSlice(context, chunk, frame, 0U, 200U, 1U, false, false);

  ScriptSuccessfulCancel(context, &sink);
  const auto cancelled = committer.Cancel(10U, generation);
  BOOMPI_EXPECT(context, cancelled.acknowledged);
  BOOMPI_EXPECT(context,
                cancelled.accepted_generation_sample_frames == 200U);
  BOOMPI_EXPECT(
      context, cancelled.accepted_after_cancel_fence_sample_frames == 200U);
  BOOMPI_EXPECT(context, cancelled.discarded_pending_sample_frames == 760U);
}

void TestTypedWriteFailuresRequireCancel(
    boompi::test::TestContext& context) {
  struct FailureCase final {
    PcmPlaybackWriteCode write_code;
    PlaybackCommitCode commit_code;
  };
  constexpr std::array<FailureCase, 6U> kCases{{
      {PcmPlaybackWriteCode::kXrun, PlaybackCommitCode::kXrun},
      {PcmPlaybackWriteCode::kSuspended, PlaybackCommitCode::kSuspended},
      {PcmPlaybackWriteCode::kDeviceLost, PlaybackCommitCode::kDeviceLost},
      {PcmPlaybackWriteCode::kZeroProgress,
       PlaybackCommitCode::kZeroProgress},
      {PcmPlaybackWriteCode::kFatal, PlaybackCommitCode::kSinkFault},
      {PcmPlaybackWriteCode::kUnavailable,
       PlaybackCommitCode::kSinkFault},
  }};

  for (std::size_t index = 0U; index < kCases.size(); ++index) {
    const auto generation = Generation(
        static_cast<std::uint32_t>(40U + index),
        static_cast<std::uint32_t>(50U + index),
        static_cast<std::uint32_t>(60U + index));
    PlaybackEpochFence fence;
    ScriptedPcmPlaybackSink sink;
    AcceptedRenderQueue queue;
    PlaybackCommitter committer;
    if (!ConfigureAndArm(context, PlaybackCommitterConfig{}, &fence, &sink,
                         &queue, generation, &committer)) {
      return;
    }
    BOOMPI_EXPECT(context,
                  sink.PushWriteCode(kCases[index].write_code, -5));
    BOOMPI_EXPECT(context,
                  committer.Submit(MakeFrame(generation, 0U, 9000000U))
                      .accepted());
    const auto result = committer.PumpOnce();
    BOOMPI_EXPECT(context, result.code == kCases[index].commit_code);
    BOOMPI_EXPECT(context, result.native_error == -5);
    BOOMPI_EXPECT(context, result.requires_cancel());
    BOOMPI_EXPECT(context, !result.made_progress());
    BOOMPI_EXPECT(context, !queue.TryAcquireRead());
    BOOMPI_EXPECT(context, fence.AdvanceTo(generation.epoch + 100U).ok());
    ScriptSuccessfulCancel(context, &sink);
    BOOMPI_EXPECT(context, committer.Cancel(index + 1U, generation).acknowledged);
  }
}

void TestMalformedPositiveWriteIsNeverReplayed(
    boompi::test::TestContext& context) {
  const auto generation = Generation(65U, 66U, 67U);
  PlaybackEpochFence fence;
  ScriptedPcmPlaybackSink sink;
  AcceptedRenderQueue queue;
  PlaybackCommitter committer;
  if (!ConfigureAndArm(context, PlaybackCommitterConfig{}, &fence, &sink,
                       &queue, generation, &committer)) {
    return;
  }

  const auto frame = MakeFrame(generation, 0U, 9500000U, 3500);
  BOOMPI_EXPECT(context, committer.Submit(frame).accepted());
  BOOMPI_EXPECT(
      context,
      sink.PushUncheckedWriteResultForProtocolTest(
          {PcmPlaybackWriteCode::kAccepted, 200U, 3000U, 2000U, 0U,
           PlaybackTimingSource::kAlsaStatus, -74}));

  const auto result = committer.PumpOnce();
  BOOMPI_EXPECT(
      context,
      result.code == PlaybackCommitCode::kAcceptedThenSinkProtocolFault);
  BOOMPI_EXPECT(context, result.made_progress());
  BOOMPI_EXPECT(context, result.requires_cancel());
  BOOMPI_EXPECT(context, result.accepted_sample_frames == 200U);
  BOOMPI_EXPECT(context, result.accepted_from_current_chunk == 200U);
  BOOMPI_EXPECT(context, result.remaining_in_current_chunk == 760U);
  BOOMPI_EXPECT(context, result.generation_accepted_sample_frames == 200U);
  BOOMPI_EXPECT(context, result.accepted_chunk_sequence == 1U);
  BOOMPI_EXPECT(context, result.native_error == -74);
  BOOMPI_EXPECT(context, sink.write_call_count() == 1U);
  BOOMPI_EXPECT(context, !queue.TryAcquireRead());

  BOOMPI_EXPECT(context, fence.AdvanceTo(66U).ok());
  ScriptSuccessfulCancel(context, &sink);
  const auto cancelled = committer.Cancel(7U, generation);
  BOOMPI_EXPECT(context, cancelled.acknowledged);
  BOOMPI_EXPECT(context,
                cancelled.accepted_generation_sample_frames == 200U);
  BOOMPI_EXPECT(context, cancelled.discarded_pending_sample_frames == 760U);
}

void TestOverAcceptAndControlFailures(
    boompi::test::TestContext& context) {
  {
    const auto generation = Generation(70U, 71U, 72U);
    PlaybackEpochFence fence;
    ScriptedPcmPlaybackSink sink;
    AcceptedRenderQueue queue;
    PlaybackCommitter committer;
    if (!ConfigureAndArm(context, PlaybackCommitterConfig{}, &fence, &sink,
                         &queue, generation, &committer)) {
      return;
    }
    BOOMPI_EXPECT(context,
                  sink.PushAccepted(961U, 1000U, 2000U, 0U,
                                    PlaybackTimingSource::kSoftwareEstimate));
    BOOMPI_EXPECT(context,
                  committer.Submit(MakeFrame(generation, 0U, 10000000U))
                      .accepted());
    const auto result = committer.PumpOnce();
    BOOMPI_EXPECT(
        context,
        result.code == PlaybackCommitCode::kAcceptedThenSinkProtocolFault);
    BOOMPI_EXPECT(context, result.requires_cancel());
    BOOMPI_EXPECT(context, result.accepted_sample_frames == 960U);
    BOOMPI_EXPECT(context, result.generation_accepted_sample_frames == 960U);
    BOOMPI_EXPECT(context, !queue.TryAcquireRead());
  }

  {
    const auto generation = Generation(73U, 74U, 75U);
    PlaybackEpochFence fence;
    ScriptedPcmPlaybackSink sink;
    AcceptedRenderQueue queue;
    PlaybackCommitter committer;
    if (!ConfigureAndArm(context, PlaybackCommitterConfig{}, &fence, &sink,
                         &queue, generation, &committer)) {
      return;
    }
    sink.ResetTrace();
    BOOMPI_EXPECT(context,
                  sink.PushDropResult({PcmPlaybackControlCode::kFatal, -5}));
    BOOMPI_EXPECT(context, fence.AdvanceTo(74U).ok());
    const auto result = committer.Cancel(1U, generation);
    BOOMPI_EXPECT(context, result.code == PlaybackCancelCode::kDropFailed);
    BOOMPI_EXPECT(context, !result.acknowledged);
    BOOMPI_EXPECT(context, sink.drop_call_count() == 1U);
    BOOMPI_EXPECT(context, sink.prepare_call_count() == 0U);
    BOOMPI_EXPECT(context,
                  committer.state() == PlaybackCommitterState::kSinkFault);
  }

  {
    const auto generation = Generation(76U, 77U, 78U);
    PlaybackEpochFence fence;
    ScriptedPcmPlaybackSink sink;
    AcceptedRenderQueue queue;
    PlaybackCommitter committer;
    if (!ConfigureAndArm(context, PlaybackCommitterConfig{}, &fence, &sink,
                         &queue, generation, &committer)) {
      return;
    }
    sink.ResetTrace();
    BOOMPI_EXPECT(
        context,
        sink.PushDropResult({PcmPlaybackControlCode::kSucceeded, 0}));
    BOOMPI_EXPECT(
        context,
        sink.PushPrepareResult({PcmPlaybackControlCode::kFatal, -6}));
    BOOMPI_EXPECT(context, fence.AdvanceTo(77U).ok());
    const auto result = committer.Cancel(1U, generation);
    BOOMPI_EXPECT(context, result.code == PlaybackCancelCode::kPrepareFailed);
    BOOMPI_EXPECT(context, result.drop_succeeded);
    BOOMPI_EXPECT(context, !result.prepare_succeeded);
    BOOMPI_EXPECT(context, !result.acknowledged);
    BOOMPI_EXPECT(context, sink.drop_call_count() == 1U);
    BOOMPI_EXPECT(context, sink.prepare_call_count() == 1U);
  }

  {
    const auto generation = Generation(79U, 80U, 81U);
    PlaybackEpochFence fence;
    ScriptedPcmPlaybackSink sink;
    AcceptedRenderQueue queue;
    PlaybackCommitter committer;
    if (!ConfigureAndArm(context, PlaybackCommitterConfig{}, &fence, &sink,
                         &queue, generation, &committer)) {
      return;
    }
    sink.ResetTrace();
    BOOMPI_EXPECT(
        context, sink.PushUncheckedDropResultForProtocolTest({}));
    BOOMPI_EXPECT(context, fence.AdvanceTo(80U).ok());
    const auto result = committer.Cancel(1U, generation);
    BOOMPI_EXPECT(context, result.code == PlaybackCancelCode::kDropFailed);
    BOOMPI_EXPECT(context, !result.acknowledged);
    BOOMPI_EXPECT(context, sink.drop_call_count() == 1U);
    BOOMPI_EXPECT(context, sink.prepare_call_count() == 0U);
  }

  {
    const auto generation = Generation(82U, 83U, 84U);
    PlaybackEpochFence fence;
    ScriptedPcmPlaybackSink sink;
    AcceptedRenderQueue queue;
    PlaybackCommitter committer;
    if (!ConfigureAndArm(context, PlaybackCommitterConfig{}, &fence, &sink,
                         &queue, generation, &committer)) {
      return;
    }
    sink.ResetTrace();
    BOOMPI_EXPECT(
        context,
        sink.PushDropResult({PcmPlaybackControlCode::kSucceeded, 0}));
    BOOMPI_EXPECT(
        context, sink.PushUncheckedPrepareResultForProtocolTest({}));
    BOOMPI_EXPECT(context, fence.AdvanceTo(83U).ok());
    const auto result = committer.Cancel(1U, generation);
    BOOMPI_EXPECT(context, result.code == PlaybackCancelCode::kPrepareFailed);
    BOOMPI_EXPECT(context, result.drop_succeeded);
    BOOMPI_EXPECT(context, !result.prepare_succeeded);
    BOOMPI_EXPECT(context, !result.acknowledged);
  }
}

void TestEosPrefixAndDrainPartialOrdering(
    boompi::test::TestContext& context) {
  const auto generation = Generation(80U, 81U, 82U);
  PlaybackEpochFence fence;
  ScriptedPcmPlaybackSink sink;
  AcceptedRenderQueue queue;
  PlaybackCommitter committer;
  if (!ConfigureAndArm(context, PlaybackCommitterConfig{}, &fence, &sink,
                       &queue, generation, &committer)) {
    return;
  }

  const auto prefix = MakeFrame(generation, 0U, 11000000U, 4000, 146U);
  BOOMPI_EXPECT(context, prefix.HasValidLength());
  BOOMPI_EXPECT(context, sink.PushAccepted(
                             100U, 1000U, 2000U, 0U,
                             PlaybackTimingSource::kSoftwareEstimate));
  BOOMPI_EXPECT(context, sink.PushAccepted(
                             46U, 1100U, 2100U, 100U,
                             PlaybackTimingSource::kSoftwareEstimate));
  BOOMPI_EXPECT(context, committer.Submit(prefix).accepted());
  BOOMPI_EXPECT(context,
                committer.PumpOnce().code ==
                    PlaybackCommitCode::kAcceptedPartial);
  BOOMPI_EXPECT(context,
                committer.PumpOnce().code ==
                    PlaybackCommitCode::kChunkAccepted);

  const auto drain = MakeFrame(generation, 0U, 11000000U, 5000, 63U,
                               146U, true);
  BOOMPI_EXPECT(context, drain.HasValidLength());
  BOOMPI_EXPECT(context, sink.PushAccepted(
                             20U, 1200U, 2200U, 146U,
                             PlaybackTimingSource::kAlsaStatus));
  BOOMPI_EXPECT(context, sink.PushAccepted(
                             43U, 1300U, 2300U, 166U,
                             PlaybackTimingSource::kAlsaStatus));
  BOOMPI_EXPECT(context, committer.Submit(drain).accepted());
  BOOMPI_EXPECT(context,
                committer.PumpOnce().code ==
                    PlaybackCommitCode::kAcceptedPartial);
  const auto final_result = committer.PumpOnce();
  BOOMPI_EXPECT(context,
                final_result.code == PlaybackCommitCode::kEndOfStreamAccepted);
  BOOMPI_EXPECT(context, final_result.end_of_stream_accepted);
  BOOMPI_EXPECT(context,
                committer.state() ==
                    PlaybackCommitterState::kEndOfStreamAccepted);

  constexpr std::array<std::uint16_t, 4U> kOffsets{{0U, 100U, 146U, 166U}};
  constexpr std::array<std::uint16_t, 4U> kLengths{{100U, 46U, 20U, 43U}};
  for (std::size_t index = 0U; index < kOffsets.size(); ++index) {
    AcceptedRenderChunk48k chunk{};
    BOOMPI_EXPECT(context, PopAccepted(&queue, &chunk));
    BOOMPI_EXPECT(
        context,
        chunk.absolute_source_offset_sample_frames == kOffsets[index]);
    BOOMPI_EXPECT(context, chunk.accepted_samples == kLengths[index]);
    BOOMPI_EXPECT(context,
                  chunk.source_end_of_stream == (index == 3U));
    BOOMPI_EXPECT(context,
                  chunk.completes_render_chunk ==
                      (index == 1U || index == 3U));
  }
  BOOMPI_EXPECT(
      context,
      committer.PumpOnce().code ==
          PlaybackCommitCode::kEndOfStreamAlreadyAccepted);
  BOOMPI_EXPECT(
      context,
      committer.Submit(MakeFrame(generation, 1U, 11020000U)).code ==
          PlaybackSubmitCode::kEndOfStreamAlreadyAccepted);
  BOOMPI_EXPECT(context, fence.AdvanceTo(81U).ok());
  ScriptSuccessfulCancel(context, &sink);
  BOOMPI_EXPECT(context, committer.Cancel(1U, generation).acknowledged);
}

void TestGenerationOrderingAndHardwareReference(
    boompi::test::TestContext& context) {
  {
    const auto generation = Generation(90U, 91U, 92U);
    PlaybackEpochFence fence;
    ScriptedPcmPlaybackSink sink;
    AcceptedRenderQueue queue;
    PlaybackCommitter committer;
    if (!ConfigureAndArm(context, PlaybackCommitterConfig{}, &fence, &sink,
                         &queue, generation, &committer)) {
      return;
    }
    auto stale = MakeFrame(generation, 0U, 12000000U);
    stale.metadata.epoch += 1U;
    BOOMPI_EXPECT(context,
                  committer.Submit(stale).code ==
                      PlaybackSubmitCode::kEpochMismatch);
    const auto first = MakeFrame(generation, 0U, 12000000U);
    BOOMPI_EXPECT(context, sink.PushAccepted(
                               960U, 1000U, 2000U, 0U,
                               PlaybackTimingSource::kSoftwareEstimate));
    BOOMPI_EXPECT(context, committer.Submit(first).accepted());
    BOOMPI_EXPECT(context,
                  committer.PumpOnce().code ==
                      PlaybackCommitCode::kChunkAccepted);
    BOOMPI_EXPECT(context,
                  committer.Submit(first).code ==
                      PlaybackSubmitCode::kOrderingFault);
    BOOMPI_EXPECT(context, fence.AdvanceTo(91U).ok());
    ScriptSuccessfulCancel(context, &sink);
    BOOMPI_EXPECT(context, committer.Cancel(1U, generation).acknowledged);
  }

  {
    const auto generation = Generation(100U, 101U, 102U);
    PlaybackEpochFence fence;
    ScriptedPcmPlaybackSink sink;
    PlaybackCommitter committer;
    PlaybackCommitterConfig config{};
    config.reference_policy =
        PlaybackReferencePolicy::kHardwareCaptureReference;
    if (!ConfigureAndArm(context, config, &fence, &sink, nullptr, generation,
                         &committer)) {
      return;
    }
    BOOMPI_EXPECT(context, sink.PushAccepted(
                               960U, 1000U, 2000U, 0U,
                               PlaybackTimingSource::kAlsaStatus));
    BOOMPI_EXPECT(context,
                  committer.Submit(MakeFrame(generation, 0U, 13000000U))
                      .accepted());
    BOOMPI_EXPECT(context,
                  committer.PumpOnce().code ==
                      PlaybackCommitCode::kChunkAccepted);
    BOOMPI_EXPECT(context, fence.AdvanceTo(101U).ok());
    ScriptSuccessfulCancel(context, &sink);
    BOOMPI_EXPECT(context, committer.Cancel(1U, generation).acknowledged);
  }
}

void TestAcceptedIdentityAdvancesAcrossCancellation(
    boompi::test::TestContext& context) {
  const auto old_generation = Generation(105U, 106U, 107U);
  const auto new_generation = Generation(106U, 108U, 109U);
  PlaybackEpochFence fence;
  ScriptedPcmPlaybackSink sink;
  AcceptedRenderQueue queue;
  PlaybackCommitter committer;
  if (!ConfigureAndArm(context, PlaybackCommitterConfig{}, &fence, &sink,
                       &queue, old_generation, &committer)) {
    return;
  }

  const auto old_frame = MakeFrame(old_generation, 0U, 13500000U, 6000);
  BOOMPI_EXPECT(context, sink.PushAccepted(
                             960U, 1000U, 2000U, 0U,
                             PlaybackTimingSource::kSoftwareEstimate));
  BOOMPI_EXPECT(context, committer.Submit(old_frame).accepted());
  BOOMPI_EXPECT(context,
                committer.PumpOnce().code ==
                    PlaybackCommitCode::kChunkAccepted);
  AcceptedRenderChunk48k accepted{};
  BOOMPI_EXPECT(context, PopAccepted(&queue, &accepted));
  BOOMPI_EXPECT(context, accepted.pcm_incarnation == 1U);
  BOOMPI_EXPECT(context, accepted.accepted_chunk_sequence == 1U);

  BOOMPI_EXPECT(context, fence.AdvanceTo(new_generation.epoch).ok());
  ScriptSuccessfulCancel(context, &sink);
  const auto cancelled = committer.Cancel(55U, old_generation);
  BOOMPI_EXPECT(context, cancelled.acknowledged);
  BOOMPI_EXPECT(context, cancelled.retired_pcm_incarnation == 1U);
  BOOMPI_EXPECT(context, cancelled.prepared_pcm_incarnation == 2U);
  BOOMPI_EXPECT(
      context,
      committer.ConfirmReferenceReset(55U, old_generation).code ==
          PlaybackReferenceResetCode::kConfirmed);
  BOOMPI_EXPECT(context, committer.Arm(new_generation).ok());

  const auto new_frame = MakeFrame(new_generation, 0U, 13600000U, 7000);
  BOOMPI_EXPECT(context, sink.PushAccepted(
                             960U, 3000U, 4000U, 0U,
                             PlaybackTimingSource::kAlsaStatus));
  BOOMPI_EXPECT(context, committer.Submit(new_frame).accepted());
  BOOMPI_EXPECT(context,
                committer.PumpOnce().code ==
                    PlaybackCommitCode::kChunkAccepted);
  BOOMPI_EXPECT(context, PopAccepted(&queue, &accepted));
  BOOMPI_EXPECT(context, accepted.pcm_incarnation == 2U);
  BOOMPI_EXPECT(context, accepted.accepted_chunk_sequence == 2U);

  const std::uint32_t drops_before_late_cancel = sink.drop_call_count();
  BOOMPI_EXPECT(
      context,
      committer.Cancel(56U, old_generation).code ==
          PlaybackCancelCode::kGenerationMismatch);
  BOOMPI_EXPECT(context,
                sink.drop_call_count() == drops_before_late_cancel);
}

void TestIdentityExhaustionFailsBeforeNextWrite(
    boompi::test::TestContext& context) {
  {
    const auto generation = Generation(110U, 111U, 112U);
    PlaybackEpochFence fence;
    ScriptedPcmPlaybackSink sink;
    AcceptedRenderQueue queue;
    PlaybackCommitter committer;
    PlaybackCommitterConfig config{};
    config.first_accepted_chunk_sequence =
        std::numeric_limits<std::uint64_t>::max();
    if (!ConfigureAndArm(context, config, &fence, &sink, &queue, generation,
                         &committer)) {
      return;
    }
    BOOMPI_EXPECT(context, sink.PushAccepted(
                               960U, 1000U, 2000U, 0U,
                               PlaybackTimingSource::kSoftwareEstimate));
    BOOMPI_EXPECT(context,
                  committer.Submit(MakeFrame(generation, 0U, 14000000U))
                      .accepted());
    const auto first = committer.PumpOnce();
    BOOMPI_EXPECT(context,
                  first.accepted_chunk_sequence ==
                      std::numeric_limits<std::uint64_t>::max());
    const auto exhausted =
        committer.Submit(MakeFrame(generation, 1U, 14020000U));
    BOOMPI_EXPECT(
        context, exhausted.code == PlaybackSubmitCode::kRestartRequired);
    BOOMPI_EXPECT(context, exhausted.requires_cancel());
    BOOMPI_EXPECT(context, sink.write_call_count() == 1U);
    BOOMPI_EXPECT(context, fence.AdvanceTo(111U).ok());
    ScriptSuccessfulCancel(context, &sink);
    const auto terminal = committer.Cancel(1U, generation);
    BOOMPI_EXPECT(context,
                  terminal.code == PlaybackCancelCode::kRestartRequired);
    BOOMPI_EXPECT(context, terminal.acknowledged);
    BOOMPI_EXPECT(context, terminal.drop_succeeded);
    BOOMPI_EXPECT(context, terminal.prepare_succeeded);
    BOOMPI_EXPECT(
        context, committer.state() == PlaybackCommitterState::kSinkFault);
    BOOMPI_EXPECT(
        context,
        committer.Cancel(1U, generation).code ==
            PlaybackCancelCode::kRestartRequired);
    BOOMPI_EXPECT(
        context,
        committer.ConfirmReferenceReset(1U, generation).code ==
            PlaybackReferenceResetCode::kSinkFault);
    BOOMPI_EXPECT(
        context,
        !committer.Arm(Generation(111U, 113U, 114U)).ok());
  }

  {
    const auto generation = Generation(120U, 121U, 122U);
    PlaybackEpochFence fence;
    ScriptedPcmPlaybackSink sink;
    AcceptedRenderQueue queue;
    PlaybackCommitter committer;
    PlaybackCommitterConfig config{};
    config.initial_pcm_incarnation =
        std::numeric_limits<std::uint64_t>::max();
    if (!ConfigureAndArm(context, config, &fence, &sink, &queue, generation,
                         &committer)) {
      return;
    }
    sink.ResetTrace();
    BOOMPI_EXPECT(context, fence.AdvanceTo(121U).ok());
    BOOMPI_EXPECT(
        context,
        sink.PushDropResult({PcmPlaybackControlCode::kSucceeded, 0}));
    const auto result = committer.Cancel(1U, generation);
    BOOMPI_EXPECT(
        context, result.code == PlaybackCancelCode::kIncarnationExhausted);
    BOOMPI_EXPECT(context, result.drop_succeeded);
    BOOMPI_EXPECT(context, !result.prepare_succeeded);
    BOOMPI_EXPECT(
        context,
        result.retired_pcm_incarnation ==
            std::numeric_limits<std::uint64_t>::max());
    BOOMPI_EXPECT(context, result.prepared_pcm_incarnation == 0U);
    BOOMPI_EXPECT(context, !result.acknowledged);
    BOOMPI_EXPECT(context, sink.drop_call_count() == 1U);
    BOOMPI_EXPECT(context, sink.prepare_call_count() == 0U);
  }
}

}  // namespace

int main() {
  boompi::test::TestContext context;
  TestConfigurationAndInitialization(context);
  TestPartialWritesAreExactlyOnce(context);
  TestReferenceBackpressurePreventsWrite(context);
  TestCancelBarrierAndLateCancel(context);
  TestPositiveWriteRacingCancelIsRecorded(context);
  TestTypedWriteFailuresRequireCancel(context);
  TestMalformedPositiveWriteIsNeverReplayed(context);
  TestOverAcceptAndControlFailures(context);
  TestEosPrefixAndDrainPartialOrdering(context);
  TestGenerationOrderingAndHardwareReference(context);
  TestAcceptedIdentityAdvancesAcrossCancellation(context);
  TestIdentityExhaustionFailsBeforeNextWrite(context);

  if (context.failures() != 0U) {
    std::cerr << context.failures()
              << " playback committer expectation(s) failed\n";
    return 1;
  }
  std::cout << "boomPI playback committer tests passed\n";
  return 0;
}
