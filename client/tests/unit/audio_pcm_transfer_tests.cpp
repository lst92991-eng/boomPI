#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <vector>

#include "boompi/audio/playback_committer.h"
#include "boompi/platform/audio_pcm_transfer.h"
#include "boompi/test/test_context.h"

#define BOOMPI_EXPECT(context, expression) \
  (context).Expect((expression), #expression, __FILE__, __LINE__)

namespace {

using boompi::audio::AcceptedRenderChunk48k;
using boompi::audio::AcceptedRenderQueue;
using boompi::audio::PcmPlaybackControlCode;
using boompi::audio::PcmPlaybackWriteCode;
using boompi::audio::PlaybackCancelCode;
using boompi::audio::PlaybackCommitCode;
using boompi::audio::PlaybackCommitter;
using boompi::audio::PlaybackCommitterConfig;
using boompi::audio::PlaybackGeneration;
using boompi::audio::PlaybackPcmFrame48k;
using boompi::platform::AudioIoClock;
using boompi::platform::AudioIoStopFlag;
using boompi::platform::AudioPeriodTransferConfig;
using boompi::platform::CaptureEpochFence;
using boompi::platform::CapturePeriodReader48k;
using boompi::platform::CaptureTransferCode;
using boompi::platform::PcmBackendCode;
using boompi::platform::PcmBackendResult;
using boompi::platform::PcmPlaybackSink48k;
using boompi::platform::PcmStreamBackend;

class FakeClock final : public AudioIoClock {
 public:
  explicit FakeClock(const std::uint64_t now_us) noexcept : now_us_(now_us) {}

  std::uint64_t NowUs() const noexcept override { return now_us_; }
  void AdvanceUs(const std::uint64_t delta_us) noexcept { now_us_ += delta_us; }

 private:
  std::uint64_t now_us_;
};

class ScriptedBackend final : public PcmStreamBackend {
 public:
  explicit ScriptedBackend(FakeClock* const clock) noexcept : clock_(clock) {}

  PcmBackendResult Wait(const std::uint32_t timeout_ms) noexcept override {
    ++wait_calls;
    if (clock_ != nullptr) {
      clock_->AdvanceUs(static_cast<std::uint64_t>(timeout_ms) * 1000U);
    }
    if (wait_index < wait_results.size()) {
      return wait_results[wait_index++];
    }
    return {PcmBackendCode::kTimeout, 0U};
  }

  PcmBackendResult ReadInterleaved(
      std::int16_t* const samples,
      const std::uint32_t requested_sample_frames) noexcept override {
    ++read_calls;
    last_read_request = requested_sample_frames;
    if (read_index >= read_results.size()) {
      return {PcmBackendCode::kFatal, 0U};
    }
    const PcmBackendResult result = read_results[read_index++];
    if (result.code == PcmBackendCode::kFrames && samples != nullptr) {
      const std::size_t count =
          static_cast<std::size_t>(
              std::min(result.sample_frames, requested_sample_frames)) *
          capture_channels;
      for (std::size_t sample = 0U; sample < count; ++sample) {
        samples[sample] = static_cast<std::int16_t>(sample % 30000U);
      }
    }
    return result;
  }

  PcmBackendResult WriteInterleaved(
      const std::int16_t* const samples,
      const std::uint32_t requested_sample_frames) noexcept override {
    ++write_calls;
    write_requests.push_back(requested_sample_frames);
    if (samples != nullptr && requested_sample_frames > 0U) {
      first_left_samples.push_back(samples[0]);
      first_right_samples.push_back(samples[1]);
    }
    if (write_index >= write_results.size()) {
      return {PcmBackendCode::kFatal, 0U};
    }
    return write_results[write_index++];
  }

  bool Recover(const PcmBackendCode reason) noexcept override {
    ++recover_calls;
    last_recover_reason = reason;
    return recover_succeeds;
  }

  bool Drop() noexcept override {
    ++drop_calls;
    return drop_succeeds;
  }

  bool Prepare() noexcept override {
    ++prepare_calls;
    return prepare_succeeds;
  }

  bool Reset() noexcept override {
    ++reset_calls;
    return reset_succeeds;
  }

  bool QueryQueuedSampleFrames(
      std::uint32_t* const queued_sample_frames) noexcept override {
    ++query_calls;
    if (!query_succeeds || queued_sample_frames == nullptr) {
      return false;
    }
    *queued_sample_frames = queued_sample_frames_value;
    return true;
  }

  FakeClock* clock_{nullptr};
  std::vector<PcmBackendResult> wait_results;
  std::vector<PcmBackendResult> read_results;
  std::vector<PcmBackendResult> write_results;
  std::vector<std::uint32_t> write_requests;
  std::vector<std::int16_t> first_left_samples;
  std::vector<std::int16_t> first_right_samples;
  std::size_t wait_index{0U};
  std::size_t read_index{0U};
  std::size_t write_index{0U};
  std::uint32_t wait_calls{0U};
  std::uint32_t read_calls{0U};
  std::uint32_t write_calls{0U};
  std::uint32_t query_calls{0U};
  std::uint32_t recover_calls{0U};
  std::uint32_t drop_calls{0U};
  std::uint32_t prepare_calls{0U};
  std::uint32_t reset_calls{0U};
  std::uint32_t last_read_request{0U};
  std::uint32_t queued_sample_frames_value{0U};
  std::uint8_t capture_channels{4U};
  PcmBackendCode last_recover_reason{PcmBackendCode::kReady};
  bool query_succeeds{true};
  bool recover_succeeds{true};
  bool drop_succeeds{true};
  bool prepare_succeeds{true};
  bool reset_succeeds{true};
};

AudioPeriodTransferConfig TransferConfig() {
  return {10U, 30U};
}

PlaybackGeneration Generation(const std::uint32_t epoch,
                              const std::uint32_t turn_id,
                              const std::uint32_t stream_id) {
  return {epoch, turn_id, stream_id};
}

PlaybackPcmFrame48k PlaybackFrame(const PlaybackGeneration& generation) {
  PlaybackPcmFrame48k frame{};
  frame.format = {PlaybackPcmFrame48k::kSampleRateHz,
                  PlaybackPcmFrame48k::kFrameDurationMs,
                  PlaybackPcmFrame48k::kChannelCount,
                  PlaybackPcmFrame48k::kSampleFormat};
  frame.metadata.monotonic_timestamp_us = 1000000U;
  frame.metadata.sequence = 3U;
  frame.metadata.epoch = generation.epoch;
  frame.metadata.turn_id = generation.turn_id;
  frame.metadata.stream_id = generation.stream_id;
  frame.valid_samples = PlaybackPcmFrame48k::kFrameSamples;
  for (std::size_t sample = 0U; sample < frame.samples.size(); ++sample) {
    frame.samples[sample] = static_cast<std::int16_t>(sample + 1U);
  }
  return frame;
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

std::array<std::int16_t, PlaybackPcmFrame48k::kFrameSamples> MonoSamples() {
  std::array<std::int16_t, PlaybackPcmFrame48k::kFrameSamples> samples{};
  for (std::size_t index = 0U; index < samples.size(); ++index) {
    samples[index] = static_cast<std::int16_t>(index + 1U);
  }
  return samples;
}

void TestPlaybackSinkAcceptedAndControls(
    boompi::test::TestContext& context) {
  FakeClock clock(2000000U);
  ScriptedBackend backend(&clock);
  backend.queued_sample_frames_value = 480U;
  backend.write_results = {{PcmBackendCode::kFrames, 320U}};
  PcmPlaybackSink48k sink(&backend, &clock);
  const auto samples = MonoSamples();

  const auto result = sink.TryWriteInterleavedS16(
      samples.data(), PlaybackPcmFrame48k::kFrameSamples);
  BOOMPI_EXPECT(context, result.code == PcmPlaybackWriteCode::kAccepted);
  BOOMPI_EXPECT(context, result.valid());
  BOOMPI_EXPECT(context, result.accepted_sample_frames == 320U);
  BOOMPI_EXPECT(context, result.write_completed_timestamp_us == 2000000U);
  BOOMPI_EXPECT(context,
                result.estimated_presentation_timestamp_us == 2010000U);
  BOOMPI_EXPECT(context, result.queued_sample_frames_before_write == 480U);
  BOOMPI_EXPECT(context, backend.write_requests[0] == 960U);
  BOOMPI_EXPECT(context, backend.first_left_samples[0] == 1);
  BOOMPI_EXPECT(context, backend.first_right_samples[0] == 1);

  const auto drop = sink.Drop();
  const auto prepare = sink.Prepare();
  BOOMPI_EXPECT(context, drop.code == PcmPlaybackControlCode::kSucceeded);
  BOOMPI_EXPECT(context, prepare.code == PcmPlaybackControlCode::kSucceeded);
  BOOMPI_EXPECT(context, backend.drop_calls == 1U);
  BOOMPI_EXPECT(context, backend.prepare_calls == 1U);
}

void TestPlaybackSinkFailClosedAndMalformed(
    boompi::test::TestContext& context) {
  const auto samples = MonoSamples();

  FakeClock clock(3000000U);
  ScriptedBackend query_failure(&clock);
  query_failure.query_succeeds = false;
  PcmPlaybackSink48k query_sink(&query_failure, &clock);
  auto result = query_sink.TryWriteInterleavedS16(samples.data(), 960U);
  BOOMPI_EXPECT(context, result.code == PcmPlaybackWriteCode::kFatal);
  BOOMPI_EXPECT(context, query_failure.write_calls == 0U);

  ScriptedBackend excessive_queue(&clock);
  excessive_queue.queued_sample_frames_value = 72001U;
  PcmPlaybackSink48k excessive_sink(&excessive_queue, &clock);
  result = excessive_sink.TryWriteInterleavedS16(samples.data(), 960U);
  BOOMPI_EXPECT(context, result.code == PcmPlaybackWriteCode::kFatal);
  BOOMPI_EXPECT(context, excessive_queue.write_calls == 0U);

  ScriptedBackend zero_progress(&clock);
  zero_progress.write_results = {{PcmBackendCode::kFrames, 0U}};
  PcmPlaybackSink48k zero_sink(&zero_progress, &clock);
  result = zero_sink.TryWriteInterleavedS16(samples.data(), 960U);
  BOOMPI_EXPECT(context,
                result.code == PcmPlaybackWriteCode::kZeroProgress);
  BOOMPI_EXPECT(context, result.valid());

  ScriptedBackend contradictory(&clock);
  contradictory.write_results = {{PcmBackendCode::kXrun, 100U}};
  PcmPlaybackSink48k contradictory_sink(&contradictory, &clock);
  result = contradictory_sink.TryWriteInterleavedS16(samples.data(), 960U);
  BOOMPI_EXPECT(context, result.code == PcmPlaybackWriteCode::kXrun);
  BOOMPI_EXPECT(context, result.accepted_sample_frames == 100U);
  BOOMPI_EXPECT(context, !result.valid());

  ScriptedBackend over_report(&clock);
  over_report.write_results = {{PcmBackendCode::kFrames, 961U}};
  PcmPlaybackSink48k over_report_sink(&over_report, &clock);
  result = over_report_sink.TryWriteInterleavedS16(samples.data(), 960U);
  BOOMPI_EXPECT(context, result.accepted_sample_frames == 961U);

  FakeClock overflow_clock(
      std::numeric_limits<std::uint64_t>::max() - 1000000U);
  ScriptedBackend overflow(&overflow_clock);
  overflow.queued_sample_frames_value = 72000U;
  overflow.write_results = {{PcmBackendCode::kFrames, 960U}};
  PcmPlaybackSink48k overflow_sink(&overflow, &overflow_clock);
  result = overflow_sink.TryWriteInterleavedS16(samples.data(), 960U);
  BOOMPI_EXPECT(context, result.code == PcmPlaybackWriteCode::kAccepted);
  BOOMPI_EXPECT(context, result.accepted_sample_frames == 960U);
  BOOMPI_EXPECT(context, !result.valid());

  PcmPlaybackSink48k unavailable(nullptr, nullptr);
  BOOMPI_EXPECT(
      context,
      unavailable.TryWriteInterleavedS16(samples.data(), 960U).code ==
          PcmPlaybackWriteCode::kFatal);
  BOOMPI_EXPECT(context,
                unavailable.Drop().code ==
                    PcmPlaybackControlCode::kUnavailable);
}

void TestPlaybackCommitterUsesPlatformSink(
    boompi::test::TestContext& context) {
  FakeClock clock(4000000U);
  ScriptedBackend backend(&clock);
  backend.queued_sample_frames_value = 480U;
  backend.write_results = {{PcmBackendCode::kFrames, 320U},
                           {PcmBackendCode::kFrames, 640U}};
  PcmPlaybackSink48k sink(&backend, &clock);
  boompi::audio::PlaybackEpochFence fence;
  AcceptedRenderQueue queue;
  PlaybackCommitter committer;
  BOOMPI_EXPECT(
      context,
      PlaybackCommitter::Create(PlaybackCommitterConfig{}, &fence, &sink,
                                &queue, &committer)
          .ok());
  BOOMPI_EXPECT(context, committer.Initialize().ok());
  const PlaybackGeneration generation = Generation(5U, 6U, 7U);
  BOOMPI_EXPECT(context, fence.AdvanceTo(generation.epoch).ok());
  BOOMPI_EXPECT(context, committer.Arm(generation).ok());

  const PlaybackPcmFrame48k frame = PlaybackFrame(generation);
  BOOMPI_EXPECT(context, committer.Submit(frame).accepted());
  BOOMPI_EXPECT(context,
                committer.PumpOnce().code ==
                    PlaybackCommitCode::kAcceptedPartial);
  BOOMPI_EXPECT(context,
                committer.PumpOnce().code ==
                    PlaybackCommitCode::kChunkAccepted);

  AcceptedRenderChunk48k first{};
  AcceptedRenderChunk48k second{};
  BOOMPI_EXPECT(context, PopAccepted(&queue, &first));
  BOOMPI_EXPECT(context, PopAccepted(&queue, &second));
  BOOMPI_EXPECT(context, first.HasValidLength());
  BOOMPI_EXPECT(context, second.HasValidLength());
  BOOMPI_EXPECT(context, first.accepted_samples == 320U);
  BOOMPI_EXPECT(context, second.accepted_samples == 640U);
  BOOMPI_EXPECT(context,
                first.absolute_source_offset_sample_frames == 0U);
  BOOMPI_EXPECT(context,
                second.absolute_source_offset_sample_frames == 320U);
  BOOMPI_EXPECT(context, first.accepted_chunk_sequence == 1U);
  BOOMPI_EXPECT(context, second.accepted_chunk_sequence == 2U);
  BOOMPI_EXPECT(context, first.samples[0] == 1);
  BOOMPI_EXPECT(context, second.samples[0] == 321);
  BOOMPI_EXPECT(context, backend.first_left_samples[0] == 1);
  BOOMPI_EXPECT(context, backend.first_right_samples[0] == 1);
  BOOMPI_EXPECT(context, backend.first_left_samples[1] == 321);
  BOOMPI_EXPECT(context, backend.first_right_samples[1] == 321);

  BOOMPI_EXPECT(context, fence.AdvanceTo(6U).ok());
  const auto cancel = committer.Cancel(1U, generation);
  BOOMPI_EXPECT(context, cancel.code == PlaybackCancelCode::kAcknowledged);
  BOOMPI_EXPECT(context, cancel.acknowledged);
  BOOMPI_EXPECT(context, backend.drop_calls == 1U);
  BOOMPI_EXPECT(context, backend.prepare_calls == 2U);
}

void TestCaptureExactAndPartialDiscard(
    boompi::test::TestContext& context) {
  FakeClock clock(7000000U);
  ScriptedBackend backend(&clock);
  backend.capture_channels = 4U;
  backend.read_results = {{PcmBackendCode::kFrames, 960U},
                          {PcmBackendCode::kFrames, 123U},
                          {PcmBackendCode::kFrames, 960U}};
  CapturePeriodReader48k reader(&backend, &clock);
  BOOMPI_EXPECT(context, reader.Configure(TransferConfig(), 4U).ok());
  CaptureEpochFence fence;
  BOOMPI_EXPECT(context, fence.AdvanceTo(1U).ok());
  BOOMPI_EXPECT(context, reader.Activate(1U, 22U, fence).ok());
  AudioIoStopFlag stop;
  boompi::audio::CaptureFrame frame{};

  auto result = reader.Read(0U, fence, stop, &frame);
  BOOMPI_EXPECT(context, result.captured());
  BOOMPI_EXPECT(context, frame.HasValidLength());
  BOOMPI_EXPECT(context, frame.metadata.sequence == 0U);
  BOOMPI_EXPECT(context, !frame.metadata.discontinuity);
  BOOMPI_EXPECT(context, frame.samples[0] == 0);
  BOOMPI_EXPECT(context, frame.samples[1] == 1);

  result = reader.Read(23U, fence, stop, &frame);
  BOOMPI_EXPECT(context,
                result.code == CaptureTransferCode::kPartialDiscarded);
  BOOMPI_EXPECT(context, result.discarded_sample_frames == 123U);
  BOOMPI_EXPECT(context, !frame.HasValidLength());

  clock.AdvanceUs(20000U);
  result = reader.Read(23U, fence, stop, &frame);
  BOOMPI_EXPECT(context, result.captured());
  BOOMPI_EXPECT(context, frame.metadata.sequence == 2U);
  BOOMPI_EXPECT(context, frame.metadata.discontinuity);
  BOOMPI_EXPECT(context, frame.metadata.turn_id == 23U);
  BOOMPI_EXPECT(context, backend.last_read_request == 960U);
}

void TestCaptureXrunOversizeAndCancellation(
    boompi::test::TestContext& context) {
  FakeClock clock(8000000U);
  ScriptedBackend backend(&clock);
  backend.read_results = {{PcmBackendCode::kXrun, 0U},
                          {PcmBackendCode::kFrames, 960U},
                          {PcmBackendCode::kFrames, 961U}};
  CapturePeriodReader48k reader(&backend, &clock);
  BOOMPI_EXPECT(context, reader.Configure(TransferConfig(), 4U).ok());
  CaptureEpochFence fence;
  BOOMPI_EXPECT(context, fence.AdvanceTo(2U).ok());
  BOOMPI_EXPECT(context, reader.Activate(2U, 24U, fence).ok());
  AudioIoStopFlag stop;
  boompi::audio::CaptureFrame frame{};

  auto result = reader.Read(0U, fence, stop, &frame);
  BOOMPI_EXPECT(context, result.code == CaptureTransferCode::kXrun);
  BOOMPI_EXPECT(context, backend.recover_calls == 1U);
  result = reader.Read(0U, fence, stop, &frame);
  BOOMPI_EXPECT(context, result.captured());
  BOOMPI_EXPECT(context, frame.metadata.sequence == 0U);
  BOOMPI_EXPECT(context, frame.metadata.discontinuity);

  result = reader.Read(0U, fence, stop, &frame);
  BOOMPI_EXPECT(context, result.code == CaptureTransferCode::kBackendFault);
  BOOMPI_EXPECT(context, backend.reset_calls == 1U);
  BOOMPI_EXPECT(context,
                reader.Read(0U, fence, stop, &frame).code ==
                    CaptureTransferCode::kNotActive);

  ScriptedBackend stopped_backend(&clock);
  CapturePeriodReader48k stopped_reader(&stopped_backend, &clock);
  BOOMPI_EXPECT(context,
                stopped_reader.Configure(TransferConfig(), 4U).ok());
  BOOMPI_EXPECT(context, fence.AdvanceTo(3U).ok());
  BOOMPI_EXPECT(context, stopped_reader.Activate(3U, 25U, fence).ok());
  AudioIoStopFlag requested_stop;
  requested_stop.RequestStop();
  result = stopped_reader.Read(0U, fence, requested_stop, &frame);
  BOOMPI_EXPECT(context, result.code == CaptureTransferCode::kCancelled);
  BOOMPI_EXPECT(context, stopped_backend.read_calls == 0U);
  BOOMPI_EXPECT(context, stopped_backend.reset_calls == 1U);

  ScriptedBackend failed_recovery_backend(&clock);
  failed_recovery_backend.recover_succeeds = false;
  failed_recovery_backend.read_results = {{PcmBackendCode::kXrun, 0U}};
  CapturePeriodReader48k failed_recovery_reader(
      &failed_recovery_backend, &clock);
  BOOMPI_EXPECT(
      context,
      failed_recovery_reader.Configure(TransferConfig(), 4U).ok());
  BOOMPI_EXPECT(context, fence.AdvanceTo(4U).ok());
  BOOMPI_EXPECT(context,
                failed_recovery_reader.Activate(4U, 27U, fence).ok());
  result = failed_recovery_reader.Read(0U, fence, stop, &frame);
  BOOMPI_EXPECT(context, result.code == CaptureTransferCode::kBackendFault);
  BOOMPI_EXPECT(context, failed_recovery_backend.recover_calls == 1U);
  BOOMPI_EXPECT(context, failed_recovery_backend.reset_calls == 1U);
}

void TestCaptureWaitTimeoutMarksNextFrame(
    boompi::test::TestContext& context) {
  FakeClock clock(9000000U);
  ScriptedBackend backend(&clock);
  backend.read_results = {{PcmBackendCode::kWouldBlock, 0U},
                          {PcmBackendCode::kWouldBlock, 0U},
                          {PcmBackendCode::kFrames, 960U}};
  CapturePeriodReader48k reader(&backend, &clock);
  BOOMPI_EXPECT(context, reader.Configure({10U, 20U}, 4U).ok());
  CaptureEpochFence fence;
  BOOMPI_EXPECT(context, fence.AdvanceTo(5U).ok());
  BOOMPI_EXPECT(context, reader.Activate(5U, 26U, fence).ok());
  AudioIoStopFlag stop;
  boompi::audio::CaptureFrame frame{};
  auto result = reader.Read(0U, fence, stop, &frame);
  BOOMPI_EXPECT(context, result.code == CaptureTransferCode::kTimeout);
  BOOMPI_EXPECT(context, backend.wait_calls == 2U);
  BOOMPI_EXPECT(context, !frame.HasValidLength());

  clock.AdvanceUs(20000U);
  result = reader.Read(0U, fence, stop, &frame);
  BOOMPI_EXPECT(context, result.captured());
  BOOMPI_EXPECT(context, frame.metadata.sequence == 0U);
  BOOMPI_EXPECT(context, frame.metadata.discontinuity);
}

}  // namespace

int main() {
  boompi::test::TestContext context;
  TestPlaybackSinkAcceptedAndControls(context);
  TestPlaybackSinkFailClosedAndMalformed(context);
  TestPlaybackCommitterUsesPlatformSink(context);
  TestCaptureExactAndPartialDiscard(context);
  TestCaptureXrunOversizeAndCancellation(context);
  TestCaptureWaitTimeoutMarksNextFrame(context);

  if (context.failures() != 0U) {
    std::cerr << context.failures()
              << " ALSA PCM transfer expectation(s) failed\n";
    return 1;
  }
  std::cout << "boomPI ALSA PCM transfer tests passed\n";
  return 0;
}
