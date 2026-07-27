#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <thread>
#include <type_traits>

#include "boompi/audio/playback_buffer.h"
#include "boompi/audio/playback_generation.h"
#include "boompi/test/test_context.h"

#define BOOMPI_EXPECT(context, expression) \
  (context).Expect((expression), #expression, __FILE__, __LINE__)

namespace {

using boompi::audio::PlaybackGeneration;
using boompi::audio::PlaybackGenerationDecision;
using boompi::audio::PlaybackGenerationRetireResult;

static_assert(
    std::is_trivially_copyable<boompi::audio::TtsPcmFrame24k>::value,
    "TTS queue frames must remain trivially copyable");
static_assert(std::is_standard_layout<boompi::audio::TtsPcmFrame24k>::value,
              "TTS queue frames must remain standard-layout");
static_assert(
    std::is_trivially_copyable<
        boompi::audio::ResampledPcmFrame48k>::value,
    "wide resampled frames must remain trivially copyable");
static_assert(
    std::is_standard_layout<boompi::audio::ResampledPcmFrame48k>::value,
    "wide resampled frames must remain standard-layout");
static_assert(
    std::is_trivially_copyable<boompi::audio::PlaybackPcmFrame48k>::value,
    "software playback frames must remain trivially copyable");
static_assert(
    std::is_standard_layout<boompi::audio::PlaybackPcmFrame48k>::value,
    "software playback frames must remain standard-layout");
static_assert(
    std::is_trivially_copyable<
        boompi::audio::AcceptedRenderChunk48k>::value,
    "accepted render chunks must remain trivially copyable");
static_assert(
    std::is_standard_layout<
        boompi::audio::AcceptedRenderChunk48k>::value,
    "accepted render chunks must remain standard-layout");
static_assert(
    std::is_trivially_copyable<
        boompi::audio::RenderReferenceFrame48k>::value,
    "render reference frames must remain trivially copyable");
static_assert(
    std::is_standard_layout<
        boompi::audio::RenderReferenceFrame48k>::value,
    "render reference frames must remain standard-layout");
static_assert(std::is_trivially_copyable<PlaybackGeneration>::value,
              "playback generation must remain trivially copyable");
static_assert(
    std::is_trivially_copyable<boompi::audio::PlaybackDrainResult>::value,
    "playback drain result must remain trivially copyable");
static_assert(
    std::is_standard_layout<boompi::audio::PlaybackDrainResult>::value,
    "playback drain result must remain standard-layout");
static_assert(std::atomic<std::uint32_t>::is_always_lock_free,
              "playback epoch fence requires lock-free 32-bit atomics");
static_assert(boompi::audio::TtsIngressQueue::capacity() == 64U,
              "TTS ingress capacity changed");
static_assert(boompi::audio::AcceptedRenderQueue::capacity() == 64U,
              "accepted render capacity changed");
static_assert(boompi::audio::RenderReferenceQueue::capacity() == 16U,
              "render reference capacity changed");
static_assert(
    boompi::audio::AcceptedRenderChunk48k::kMaximumQueuedSampleFrames ==
        72000U,
    "1.5 second accepted-render queue limit changed");
static_assert(
    boompi::audio::RenderReferenceFrame48k::kMaximumQueuedSampleFrames ==
        72000U,
    "1.5 second render queue limit changed");

void FillTtsFrame(boompi::audio::TtsPcmFrame24k* const frame,
                  const std::uint32_t sequence,
                  const std::uint16_t valid_source_samples = 480U) {
  frame->format = {24000U, 20U, 1U,
                   boompi::audio::SampleFormat::kPcmS16Le};
  frame->metadata.monotonic_timestamp_us =
      static_cast<std::uint64_t>(sequence + 1U) * 20000U;
  frame->metadata.sequence = sequence;
  frame->metadata.stream_id = 3U;
  frame->metadata.turn_id = 2U;
  frame->metadata.epoch = 1U;
  frame->valid_source_samples = valid_source_samples;
  frame->end_of_stream = valid_source_samples < frame->samples.size();
  for (std::size_t index = 0U; index < frame->samples.size(); ++index) {
    frame->samples[index] =
        index < valid_source_samples
            ? static_cast<std::int16_t>(
                  static_cast<std::int32_t>((sequence + index) % 30001U) -
                  15000)
            : 0;
  }
}

void FillPlaybackPcmFrame(
    boompi::audio::PlaybackPcmFrame48k* const frame,
    const std::uint16_t valid_samples = 960U) {
  frame->format = {48000U, 20U, 1U,
                   boompi::audio::SampleFormat::kPcmS16Le};
  frame->metadata.monotonic_timestamp_us = 20000U;
  frame->metadata.sequence = 7U;
  frame->metadata.stream_id = 3U;
  frame->metadata.turn_id = 2U;
  frame->metadata.epoch = 1U;
  frame->source_offset_sample_frames = 0U;
  frame->valid_samples = valid_samples;
  frame->end_of_stream = valid_samples < frame->samples.size();
  for (std::size_t index = 0U; index < frame->samples.size(); ++index) {
    frame->samples[index] =
        index < valid_samples
            ? static_cast<std::int16_t>(
                  static_cast<std::int32_t>(index % 20001U) - 10000)
            : 0;
  }
}

void FillAcceptedRenderChunk(
    boompi::audio::AcceptedRenderChunk48k* const chunk,
    const std::uint64_t accepted_chunk_sequence = 1U,
    const std::uint16_t absolute_source_offset_sample_frames = 0U,
    const std::uint16_t accepted_samples = 960U,
    const bool completes_render_chunk = true,
    const bool source_end_of_stream = false) {
  chunk->format = {48000U, 20U, 1U,
                   boompi::audio::SampleFormat::kPcmS16Le};
  chunk->metadata.monotonic_timestamp_us =
      20000U + accepted_chunk_sequence * 20000U;
  chunk->metadata.sequence = static_cast<std::uint32_t>(
      (accepted_chunk_sequence - 1U) &
      std::numeric_limits<std::uint32_t>::max());
  chunk->metadata.stream_id = 3U;
  chunk->metadata.turn_id = 2U;
  chunk->metadata.epoch = 1U;
  chunk->pcm_incarnation = 9U;
  chunk->accepted_chunk_sequence = accepted_chunk_sequence;
  chunk->absolute_source_offset_sample_frames =
      absolute_source_offset_sample_frames;
  chunk->accepted_samples = accepted_samples;
  chunk->completes_render_chunk = completes_render_chunk;
  chunk->source_end_of_stream = source_end_of_stream;
  chunk->write_completed_timestamp_us =
      chunk->metadata.monotonic_timestamp_us + 1000U;
  chunk->estimated_presentation_timestamp_us =
      chunk->write_completed_timestamp_us + 2000U;
  chunk->queued_sample_frames_before_write = 96U;
  chunk->timing_source =
      boompi::audio::PlaybackTimingSource::kSoftwareEstimate;
  for (std::size_t index = 0U; index < chunk->samples.size(); ++index) {
    chunk->samples[index] =
        index < accepted_samples
            ? static_cast<std::int16_t>(
                  static_cast<std::int32_t>(
                      (accepted_chunk_sequence + index) % 20001U) -
                  10000)
            : 0;
  }
}

void FillResampledPcmFrame(
    boompi::audio::ResampledPcmFrame48k* const frame,
    const std::uint16_t valid_samples = 960U) {
  frame->metadata.monotonic_timestamp_us = 20000U;
  frame->metadata.sequence = 7U;
  frame->metadata.stream_id = 3U;
  frame->metadata.turn_id = 2U;
  frame->metadata.epoch = 1U;
  frame->source_offset_sample_frames = 0U;
  frame->valid_samples = valid_samples;
  frame->end_of_stream = false;
  for (std::size_t index = 0U; index < frame->samples.size(); ++index) {
    frame->samples[index] =
        index < valid_samples
            ? static_cast<std::int32_t>(index) * 257 - 70000
            : 0;
  }
}

boompi::audio::RenderReferenceFrame48k MakeRenderReferenceFrame(
    const std::uint8_t channels = 1U, const std::uint32_t sequence = 0U) {
  boompi::audio::RenderReferenceFrame48k frame{};
  frame.format = {48000U, 20U, channels,
                  boompi::audio::SampleFormat::kPcmS16Le};
  frame.metadata.monotonic_timestamp_us =
      50000U + static_cast<std::uint64_t>(sequence) * 20000U;
  frame.metadata.sequence = sequence;
  frame.metadata.stream_id = 3U;
  frame.metadata.turn_id = 0U;
  frame.metadata.epoch = 1U;
  frame.samples_per_channel = 960U;
  frame.render_completed_timestamp_us =
      frame.metadata.monotonic_timestamp_us - 1000U;
  frame.queued_sample_frames_before_write = 0U;
  frame.timing_source = boompi::audio::PlaybackTimingSource::kSoftwareEstimate;
  for (std::size_t index = 0U; index < frame.samples.size(); ++index) {
    frame.samples[index] = static_cast<std::int16_t>(
        static_cast<std::int32_t>((sequence + index) % 20001U) - 10000);
  }
  return frame;
}

void TestTtsFrameContract(boompi::test::TestContext& context) {
  boompi::audio::TtsPcmFrame24k frame{};
  FillTtsFrame(&frame, 7U);
  BOOMPI_EXPECT(context, frame.HasValidLength());
  BOOMPI_EXPECT(context, frame.sample_capacity() == 480U);
  BOOMPI_EXPECT(context, frame.valid_source_samples == 480U);
  BOOMPI_EXPECT(context, !frame.end_of_stream);

  auto invalid = frame;
  invalid.format.sample_rate_hz = 16000U;
  BOOMPI_EXPECT(context, !invalid.HasValidLength());
  invalid = frame;
  invalid.format.frame_duration_ms = 10U;
  BOOMPI_EXPECT(context, !invalid.HasValidLength());
  invalid = frame;
  invalid.format.channels = 2U;
  BOOMPI_EXPECT(context, !invalid.HasValidLength());
  invalid = frame;
  invalid.format.sample_format =
      static_cast<boompi::audio::SampleFormat>(0U);
  BOOMPI_EXPECT(context, !invalid.HasValidLength());

  invalid = frame;
  invalid.metadata.epoch = 0U;
  BOOMPI_EXPECT(context, !invalid.HasValidLength());
  invalid = frame;
  invalid.metadata.stream_id = 0U;
  BOOMPI_EXPECT(context, !invalid.HasValidLength());
  invalid = frame;
  invalid.metadata.turn_id = 0U;
  BOOMPI_EXPECT(context, !invalid.HasValidLength());
  invalid = frame;
  invalid.valid_source_samples = 0U;
  BOOMPI_EXPECT(context, !invalid.HasValidLength());
  invalid = frame;
  invalid.valid_source_samples = 481U;
  BOOMPI_EXPECT(context, !invalid.HasValidLength());

  FillTtsFrame(&frame, 8U, 137U);
  BOOMPI_EXPECT(context, frame.end_of_stream);
  BOOMPI_EXPECT(context, frame.HasValidLength());
  frame.end_of_stream = false;
  BOOMPI_EXPECT(context, !frame.HasValidLength());
  frame.end_of_stream = true;
  frame.samples[479U] = 1;
  BOOMPI_EXPECT(context, !frame.HasValidLength());
  frame.samples[479U] = 0;
  BOOMPI_EXPECT(context, frame.HasValidLength());

  FillTtsFrame(&frame, 9U);
  frame.end_of_stream = true;
  BOOMPI_EXPECT(context, frame.HasValidLength());
  frame.samples[0U] = -1234;
  frame.samples[479U] = 2345;
  frame.ResetHeader();
  BOOMPI_EXPECT(context, !frame.HasValidLength());
  BOOMPI_EXPECT(context, frame.format.sample_rate_hz == 0U);
  BOOMPI_EXPECT(context, frame.metadata.epoch == 0U);
  BOOMPI_EXPECT(context, frame.metadata.stream_id == 0U);
  BOOMPI_EXPECT(context, frame.metadata.turn_id == 0U);
  BOOMPI_EXPECT(context, frame.valid_source_samples == 0U);
  BOOMPI_EXPECT(context, !frame.end_of_stream);
  BOOMPI_EXPECT(context, frame.samples[0U] == -1234);
  BOOMPI_EXPECT(context, frame.samples[479U] == 2345);
}

void TestPlaybackPcmFrameContract(boompi::test::TestContext& context) {
  boompi::audio::PlaybackPcmFrame48k frame{};
  FillPlaybackPcmFrame(&frame);
  BOOMPI_EXPECT(context, frame.HasValidLength());
  BOOMPI_EXPECT(context, frame.sample_capacity() == 960U);
  BOOMPI_EXPECT(context, frame.valid_samples == 960U);
  BOOMPI_EXPECT(context, !frame.end_of_stream);

  auto invalid = frame;
  invalid.format.sample_rate_hz = 24000U;
  BOOMPI_EXPECT(context, !invalid.HasValidLength());
  invalid = frame;
  invalid.format.channels = 2U;
  BOOMPI_EXPECT(context, !invalid.HasValidLength());
  invalid = frame;
  invalid.metadata.epoch = 0U;
  BOOMPI_EXPECT(context, !invalid.HasValidLength());
  invalid = frame;
  invalid.metadata.stream_id = 0U;
  BOOMPI_EXPECT(context, !invalid.HasValidLength());
  invalid = frame;
  invalid.metadata.turn_id = 0U;
  BOOMPI_EXPECT(context, !invalid.HasValidLength());
  invalid = frame;
  invalid.valid_samples = 0U;
  BOOMPI_EXPECT(context, !invalid.HasValidLength());

  FillPlaybackPcmFrame(&frame, 273U);
  BOOMPI_EXPECT(context, frame.HasValidLength());
  frame.end_of_stream = false;
  BOOMPI_EXPECT(context, frame.HasValidLength());
  frame.samples.back() = 1;
  BOOMPI_EXPECT(context, !frame.HasValidLength());
  frame.samples.back() = 0;
  BOOMPI_EXPECT(context, frame.HasValidLength());

  FillPlaybackPcmFrame(&frame, 63U);
  frame.source_offset_sample_frames = 960U;
  frame.end_of_stream = true;
  BOOMPI_EXPECT(context, frame.HasValidLength());
  frame.valid_samples = 64U;
  BOOMPI_EXPECT(context, !frame.HasValidLength());
  frame.valid_samples = 63U;
  frame.source_offset_sample_frames = 961U;
  BOOMPI_EXPECT(context, !frame.HasValidLength());

  FillPlaybackPcmFrame(&frame);
  frame.source_offset_sample_frames = 1U;
  BOOMPI_EXPECT(context, !frame.HasValidLength());

  frame.samples.front() = -1234;
  frame.samples.back() = 2345;
  frame.ResetHeader();
  BOOMPI_EXPECT(context, !frame.HasValidLength());
  BOOMPI_EXPECT(context, frame.source_offset_sample_frames == 0U);
  BOOMPI_EXPECT(context, frame.valid_samples == 0U);
  BOOMPI_EXPECT(context, !frame.end_of_stream);
  BOOMPI_EXPECT(context, frame.samples.front() == -1234);
  BOOMPI_EXPECT(context, frame.samples.back() == 2345);
}

void TestAcceptedRenderChunkContract(
    boompi::test::TestContext& context) {
  boompi::audio::AcceptedRenderChunk48k chunk{};
  FillAcceptedRenderChunk(&chunk);
  BOOMPI_EXPECT(context, chunk.HasValidLength());
  BOOMPI_EXPECT(context, chunk.sample_capacity() == 960U);
  BOOMPI_EXPECT(context, chunk.accepted_samples == 960U);
  BOOMPI_EXPECT(context, chunk.completes_render_chunk);
  BOOMPI_EXPECT(context, !chunk.source_end_of_stream);

  FillAcceptedRenderChunk(&chunk, 2U, 100U, 273U, false, false);
  BOOMPI_EXPECT(context, chunk.HasValidLength());
  BOOMPI_EXPECT(context,
                chunk.absolute_source_offset_sample_frames == 100U);
  BOOMPI_EXPECT(context, chunk.accepted_samples == 273U);
  BOOMPI_EXPECT(context, chunk.samples[272U] != 0);
  BOOMPI_EXPECT(context, chunk.samples[273U] == 0);

  FillAcceptedRenderChunk(&chunk, 3U, 960U, 63U, true, true);
  chunk.queued_sample_frames_before_write =
      boompi::audio::AcceptedRenderChunk48k::
          kMaximumQueuedSampleFrames;
  chunk.timing_source = boompi::audio::PlaybackTimingSource::kAlsaStatus;
  chunk.estimated_presentation_timestamp_us =
      chunk.write_completed_timestamp_us;
  BOOMPI_EXPECT(context, chunk.HasValidLength());
  BOOMPI_EXPECT(context,
                chunk.absolute_source_offset_sample_frames +
                        chunk.accepted_samples ==
                    boompi::audio::AcceptedRenderChunk48k::
                        kMaximumSourceSpanSampleFrames);

  FillAcceptedRenderChunk(&chunk, 4U, 1022U, 1U, true, true);
  BOOMPI_EXPECT(context, chunk.HasValidLength());

  auto invalid = chunk;
  invalid.format.sample_rate_hz = 24000U;
  BOOMPI_EXPECT(context, !invalid.HasValidLength());
  invalid = chunk;
  invalid.format.frame_duration_ms = 10U;
  BOOMPI_EXPECT(context, !invalid.HasValidLength());
  invalid = chunk;
  invalid.format.channels = 2U;
  BOOMPI_EXPECT(context, !invalid.HasValidLength());
  invalid = chunk;
  invalid.format.sample_format =
      static_cast<boompi::audio::SampleFormat>(0U);
  BOOMPI_EXPECT(context, !invalid.HasValidLength());

  invalid = chunk;
  invalid.metadata.epoch = 0U;
  BOOMPI_EXPECT(context, !invalid.HasValidLength());
  invalid = chunk;
  invalid.metadata.stream_id = 0U;
  BOOMPI_EXPECT(context, !invalid.HasValidLength());
  invalid = chunk;
  invalid.metadata.turn_id = 0U;
  BOOMPI_EXPECT(context, !invalid.HasValidLength());
  invalid = chunk;
  invalid.pcm_incarnation = 0U;
  BOOMPI_EXPECT(context, !invalid.HasValidLength());
  invalid = chunk;
  invalid.accepted_chunk_sequence = 0U;
  BOOMPI_EXPECT(context, !invalid.HasValidLength());

  invalid = chunk;
  invalid.accepted_samples = 0U;
  BOOMPI_EXPECT(context, !invalid.HasValidLength());
  invalid = chunk;
  invalid.accepted_samples = 961U;
  BOOMPI_EXPECT(context, !invalid.HasValidLength());
  invalid = chunk;
  invalid.absolute_source_offset_sample_frames = 1023U;
  BOOMPI_EXPECT(context, !invalid.HasValidLength());
  FillAcceptedRenderChunk(&invalid, 5U, 960U, 64U, true, true);
  BOOMPI_EXPECT(context, !invalid.HasValidLength());

  FillAcceptedRenderChunk(&invalid, 5U, 50U, 20U, false, true);
  BOOMPI_EXPECT(context, !invalid.HasValidLength());
  invalid.source_end_of_stream = false;
  BOOMPI_EXPECT(context, invalid.HasValidLength());
  invalid.completes_render_chunk = true;
  BOOMPI_EXPECT(context, invalid.HasValidLength());

  FillAcceptedRenderChunk(&invalid, 6U, 0U, 10U, true, false);
  invalid.write_completed_timestamp_us = 0U;
  BOOMPI_EXPECT(context, !invalid.HasValidLength());
  FillAcceptedRenderChunk(&invalid, 6U, 0U, 10U, true, false);
  invalid.estimated_presentation_timestamp_us = 0U;
  BOOMPI_EXPECT(context, !invalid.HasValidLength());
  FillAcceptedRenderChunk(&invalid, 6U, 0U, 10U, true, false);
  invalid.estimated_presentation_timestamp_us =
      invalid.write_completed_timestamp_us - 1U;
  BOOMPI_EXPECT(context, !invalid.HasValidLength());
  FillAcceptedRenderChunk(&invalid, 6U, 0U, 10U, true, false);
  invalid.estimated_presentation_timestamp_us =
      invalid.write_completed_timestamp_us;
  BOOMPI_EXPECT(context, invalid.HasValidLength());

  invalid.queued_sample_frames_before_write =
      boompi::audio::AcceptedRenderChunk48k::
          kMaximumQueuedSampleFrames +
      1U;
  BOOMPI_EXPECT(context, !invalid.HasValidLength());
  FillAcceptedRenderChunk(&invalid, 6U, 0U, 10U, true, false);
  invalid.timing_source = boompi::audio::PlaybackTimingSource::kUnset;
  BOOMPI_EXPECT(context, !invalid.HasValidLength());
  invalid.timing_source =
      static_cast<boompi::audio::PlaybackTimingSource>(99U);
  BOOMPI_EXPECT(context, !invalid.HasValidLength());

  FillAcceptedRenderChunk(&invalid, 7U, 0U, 10U, true, false);
  invalid.samples[10U] = 1;
  BOOMPI_EXPECT(context, !invalid.HasValidLength());
  invalid.samples[10U] = 0;
  BOOMPI_EXPECT(context, invalid.HasValidLength());

  invalid.samples.front() = -1234;
  invalid.samples.back() = 2345;
  invalid.ResetHeader();
  BOOMPI_EXPECT(context, !invalid.HasValidLength());
  BOOMPI_EXPECT(context, invalid.format.sample_rate_hz == 0U);
  BOOMPI_EXPECT(context, invalid.metadata.epoch == 0U);
  BOOMPI_EXPECT(context, invalid.pcm_incarnation == 0U);
  BOOMPI_EXPECT(context, invalid.accepted_chunk_sequence == 0U);
  BOOMPI_EXPECT(
      context, invalid.absolute_source_offset_sample_frames == 0U);
  BOOMPI_EXPECT(context, invalid.accepted_samples == 0U);
  BOOMPI_EXPECT(context, !invalid.completes_render_chunk);
  BOOMPI_EXPECT(context, !invalid.source_end_of_stream);
  BOOMPI_EXPECT(context, invalid.write_completed_timestamp_us == 0U);
  BOOMPI_EXPECT(
      context, invalid.estimated_presentation_timestamp_us == 0U);
  BOOMPI_EXPECT(context,
                invalid.queued_sample_frames_before_write == 0U);
  BOOMPI_EXPECT(
      context,
      invalid.timing_source == boompi::audio::PlaybackTimingSource::kUnset);
  BOOMPI_EXPECT(context, invalid.samples.front() == -1234);
  BOOMPI_EXPECT(context, invalid.samples.back() == 2345);
}

void TestResampledPcmFrameContract(
    boompi::test::TestContext& context) {
  boompi::audio::ResampledPcmFrame48k frame{};
  FillResampledPcmFrame(&frame);
  BOOMPI_EXPECT(context, frame.HasValidLength());
  BOOMPI_EXPECT(context, frame.samples.front() == -70000);

  FillResampledPcmFrame(&frame, 17U);
  BOOMPI_EXPECT(context, frame.HasValidLength());
  frame.samples.back() = 1;
  BOOMPI_EXPECT(context, !frame.HasValidLength());
  frame.samples.back() = 0;
  frame.source_offset_sample_frames = 960U;
  frame.valid_samples = 63U;
  frame.end_of_stream = true;
  BOOMPI_EXPECT(context, frame.HasValidLength());
  frame.valid_samples = 64U;
  BOOMPI_EXPECT(context, !frame.HasValidLength());

  frame.samples.front() = -123456;
  frame.ResetHeader();
  BOOMPI_EXPECT(context, !frame.HasValidLength());
  BOOMPI_EXPECT(context, frame.source_offset_sample_frames == 0U);
  BOOMPI_EXPECT(context, frame.samples.front() == -123456);
}

void TestRenderReferenceFrameContract(
    boompi::test::TestContext& context) {
  auto mono = MakeRenderReferenceFrame(1U);
  BOOMPI_EXPECT(context, mono.HasValidLength());
  BOOMPI_EXPECT(context, mono.interleaved_samples() == 960U);
  BOOMPI_EXPECT(context, mono.metadata.turn_id == 0U);
  BOOMPI_EXPECT(context, mono.sample_capacity() == 1920U);

  auto stereo = MakeRenderReferenceFrame(2U);
  stereo.timing_source = boompi::audio::PlaybackTimingSource::kAlsaStatus;
  BOOMPI_EXPECT(context, stereo.HasValidLength());
  BOOMPI_EXPECT(context, stereo.interleaved_samples() == 1920U);

  auto invalid = mono;
  invalid.format.sample_rate_hz = 24000U;
  BOOMPI_EXPECT(context, !invalid.HasValidLength());
  invalid = mono;
  invalid.format.frame_duration_ms = 10U;
  BOOMPI_EXPECT(context, !invalid.HasValidLength());
  invalid = mono;
  invalid.format.channels = 0U;
  BOOMPI_EXPECT(context, !invalid.HasValidLength());
  invalid.format.channels = 3U;
  BOOMPI_EXPECT(context, !invalid.HasValidLength());
  invalid = mono;
  invalid.samples_per_channel = 959U;
  BOOMPI_EXPECT(context, !invalid.HasValidLength());
  invalid.samples_per_channel = 961U;
  BOOMPI_EXPECT(context, !invalid.HasValidLength());
  BOOMPI_EXPECT(context, invalid.interleaved_samples() == 0U);
  invalid = mono;
  invalid.format.sample_format =
      static_cast<boompi::audio::SampleFormat>(0U);
  BOOMPI_EXPECT(context, !invalid.HasValidLength());

  invalid = mono;
  invalid.metadata.epoch = 0U;
  BOOMPI_EXPECT(context, !invalid.HasValidLength());
  invalid = mono;
  invalid.metadata.stream_id = 0U;
  BOOMPI_EXPECT(context, !invalid.HasValidLength());
  invalid = mono;
  invalid.timing_source = boompi::audio::PlaybackTimingSource::kUnset;
  BOOMPI_EXPECT(context, !invalid.HasValidLength());
  invalid.timing_source = static_cast<boompi::audio::PlaybackTimingSource>(99U);
  BOOMPI_EXPECT(context, !invalid.HasValidLength());

  invalid = mono;
  invalid.metadata.monotonic_timestamp_us = 0U;
  BOOMPI_EXPECT(context, !invalid.HasValidLength());
  invalid = mono;
  invalid.render_completed_timestamp_us = 0U;
  BOOMPI_EXPECT(context, !invalid.HasValidLength());
  invalid = mono;
  invalid.metadata.monotonic_timestamp_us =
      invalid.render_completed_timestamp_us - 1U;
  BOOMPI_EXPECT(context, !invalid.HasValidLength());
  invalid.metadata.monotonic_timestamp_us =
      invalid.render_completed_timestamp_us;
  BOOMPI_EXPECT(context, invalid.HasValidLength());

  invalid = mono;
  invalid.queued_sample_frames_before_write =
      boompi::audio::RenderReferenceFrame48k::kMaximumQueuedSampleFrames;
  BOOMPI_EXPECT(context, invalid.HasValidLength());
  ++invalid.queued_sample_frames_before_write;
  BOOMPI_EXPECT(context, !invalid.HasValidLength());

  stereo.samples[0U] = -3210;
  stereo.samples[1919U] = 4321;
  stereo.ResetHeader();
  BOOMPI_EXPECT(context, !stereo.HasValidLength());
  BOOMPI_EXPECT(context, stereo.interleaved_samples() == 0U);
  BOOMPI_EXPECT(context, stereo.samples_per_channel == 0U);
  BOOMPI_EXPECT(context, stereo.render_completed_timestamp_us == 0U);
  BOOMPI_EXPECT(context, stereo.queued_sample_frames_before_write == 0U);
  BOOMPI_EXPECT(
      context,
      stereo.timing_source == boompi::audio::PlaybackTimingSource::kUnset);
  BOOMPI_EXPECT(context, stereo.samples[0U] == -3210);
  BOOMPI_EXPECT(context, stereo.samples[1919U] == 4321);
}

void TestGenerationValidityAndGate(boompi::test::TestContext& context) {
  const PlaybackGeneration generation{1U, 10U, 100U};
  BOOMPI_EXPECT(context, generation.valid());
  BOOMPI_EXPECT(context, !(PlaybackGeneration{0U, 10U, 100U}.valid()));
  BOOMPI_EXPECT(context, !(PlaybackGeneration{1U, 0U, 100U}.valid()));
  BOOMPI_EXPECT(context, !(PlaybackGeneration{1U, 10U, 0U}.valid()));

  boompi::audio::PlaybackGenerationGate gate;
  BOOMPI_EXPECT(context,
                gate.Check(PlaybackGeneration{}) ==
                    PlaybackGenerationDecision::kInvalidGeneration);
  BOOMPI_EXPECT(context,
                gate.Check(generation) ==
                    PlaybackGenerationDecision::kNotActive);
  BOOMPI_EXPECT(context, !gate.Activate(PlaybackGeneration{}, 1U).ok());
  BOOMPI_EXPECT(context, !gate.Activate(generation, 0U).ok());
  BOOMPI_EXPECT(context, !gate.Activate(generation, 2U).ok());
  BOOMPI_EXPECT(context, gate.Activate(generation, 1U).ok());

  const PlaybackGeneration direct_switch{2U, 20U, 200U};
  BOOMPI_EXPECT(context, !gate.Activate(direct_switch, 2U).ok());
  BOOMPI_EXPECT(context,
                gate.Check(generation) ==
                    PlaybackGenerationDecision::kAccepted);

  BOOMPI_EXPECT(context,
                gate.Check({2U, 11U, 101U}) ==
                    PlaybackGenerationDecision::kEpochMismatch);
  BOOMPI_EXPECT(context,
                gate.Check({1U, 11U, 101U}) ==
                    PlaybackGenerationDecision::kStreamMismatch);
  BOOMPI_EXPECT(context,
                gate.Check({1U, 11U, 100U}) ==
                    PlaybackGenerationDecision::kTurnMismatch);
  BOOMPI_EXPECT(context,
                gate.Check(generation) ==
                    PlaybackGenerationDecision::kAccepted);

  const PlaybackGeneration wrong_retire{1U, 11U, 100U};
  BOOMPI_EXPECT(
      context,
      gate.Retire(wrong_retire) ==
          PlaybackGenerationRetireResult::kExpectedGenerationMismatch);
  BOOMPI_EXPECT(context,
                gate.Check(generation) ==
                    PlaybackGenerationDecision::kAccepted);
  BOOMPI_EXPECT(context,
                gate.Retire(generation) ==
                    PlaybackGenerationRetireResult::kRetired);
  BOOMPI_EXPECT(context,
                gate.Retire(generation) ==
                    PlaybackGenerationRetireResult::kNotActive);
  BOOMPI_EXPECT(context,
                gate.Check(generation) ==
                    PlaybackGenerationDecision::kNotActive);

  const PlaybackGeneration retired_pair_new_turn{1U, 99U, 100U};
  BOOMPI_EXPECT(context,
                !gate.Activate(retired_pair_new_turn, 1U).ok());
  const PlaybackGeneration same_epoch_new_stream{1U, 12U, 101U};
  BOOMPI_EXPECT(context, gate.Activate(same_epoch_new_stream, 1U).ok());
  BOOMPI_EXPECT(
      context,
      gate.InvalidateIfEpochChanged(1U) ==
          PlaybackGenerationRetireResult::kEpochUnchanged);
  BOOMPI_EXPECT(context,
                gate.Check(same_epoch_new_stream) ==
                    PlaybackGenerationDecision::kAccepted);

  // A late retire for the old generation must not retire the new active one.
  BOOMPI_EXPECT(
      context,
      gate.Retire(generation) ==
          PlaybackGenerationRetireResult::kExpectedGenerationMismatch);
  BOOMPI_EXPECT(context,
                gate.Check(same_epoch_new_stream) ==
                    PlaybackGenerationDecision::kAccepted);
  BOOMPI_EXPECT(
      context,
      gate.InvalidateIfEpochChanged(2U) ==
          PlaybackGenerationRetireResult::kRetired);
  BOOMPI_EXPECT(context,
                gate.Check(same_epoch_new_stream) ==
                    PlaybackGenerationDecision::kNotActive);
  BOOMPI_EXPECT(
      context,
      gate.InvalidateIfEpochChanged(2U) ==
          PlaybackGenerationRetireResult::kNotActive);

  const PlaybackGeneration latest_retired_new_turn{1U, 77U, 101U};
  BOOMPI_EXPECT(context,
                !gate.Activate(latest_retired_new_turn, 1U).ok());
  BOOMPI_EXPECT(context, !gate.Activate(direct_switch, 1U).ok());
  BOOMPI_EXPECT(context, gate.Activate(direct_switch, 2U).ok());
  BOOMPI_EXPECT(context,
                gate.Retire(direct_switch) ==
                    PlaybackGenerationRetireResult::kRetired);

  const PlaybackGeneration zero_invalidated{3U, 30U, 300U};
  BOOMPI_EXPECT(context, gate.Activate(zero_invalidated, 3U).ok());
  BOOMPI_EXPECT(
      context,
      gate.InvalidateIfEpochChanged(0U) ==
          PlaybackGenerationRetireResult::kRetired);
  BOOMPI_EXPECT(context,
                gate.Check(zero_invalidated) ==
                    PlaybackGenerationDecision::kNotActive);
}

void TestPlaybackEpochFenceValidation(
    boompi::test::TestContext& context) {
  boompi::audio::PlaybackEpochFence fence;
  BOOMPI_EXPECT(context, fence.is_lock_free());
  BOOMPI_EXPECT(context, fence.Observe() == 0U);
  auto status = fence.AdvanceTo(0U);
  BOOMPI_EXPECT(context, !status.ok());
  BOOMPI_EXPECT(context, status.code() == boompi::StatusCode::kInvalidArgument);
  BOOMPI_EXPECT(context, fence.AdvanceTo(1U).ok());
  BOOMPI_EXPECT(context, fence.Observe() == 1U);
  status = fence.AdvanceTo(1U);
  BOOMPI_EXPECT(context, !status.ok());
  BOOMPI_EXPECT(context,
                status.code() == boompi::StatusCode::kFailedPrecondition);
  BOOMPI_EXPECT(context, fence.AdvanceTo(3U).ok());
  BOOMPI_EXPECT(context, !fence.AdvanceTo(2U).ok());
  BOOMPI_EXPECT(
      context,
      fence.AdvanceTo(std::numeric_limits<std::uint32_t>::max()).ok());
  BOOMPI_EXPECT(
      context,
      fence.Observe() == std::numeric_limits<std::uint32_t>::max());
  status = fence.AdvanceTo(std::numeric_limits<std::uint32_t>::max());
  BOOMPI_EXPECT(context, !status.ok());
  BOOMPI_EXPECT(context,
                status.code() == boompi::StatusCode::kResourceExhausted);
}

void TestPlaybackEpochFenceReleaseAcquire(
    boompi::test::TestContext& context) {
  boompi::audio::PlaybackEpochFence fence;
  std::atomic<std::uint32_t> ready{0U};
  std::atomic<bool> start{false};
  std::atomic<bool> writer_succeeded{false};
  std::atomic<bool> writer_done{false};
  std::uint32_t published_payload = 0U;
  std::uint32_t observed_payload = 0U;

  std::thread reader([&]() {
    ready.fetch_add(1U, std::memory_order_release);
    while (!start.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
    while (fence.Observe() != 1U) {
      if (writer_done.load(std::memory_order_acquire) &&
          !writer_succeeded.load(std::memory_order_acquire)) {
        return;
      }
      std::this_thread::yield();
    }
    observed_payload = published_payload;
  });

  std::thread writer([&]() {
    ready.fetch_add(1U, std::memory_order_release);
    while (!start.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
    published_payload = 0xA5A55A5AU;
    writer_succeeded.store(fence.AdvanceTo(1U).ok(),
                           std::memory_order_release);
    writer_done.store(true, std::memory_order_release);
  });

  while (ready.load(std::memory_order_acquire) != 2U) {
    std::this_thread::yield();
  }
  start.store(true, std::memory_order_release);
  writer.join();
  reader.join();

  BOOMPI_EXPECT(context,
                writer_succeeded.load(std::memory_order_acquire));
  BOOMPI_EXPECT(context, observed_payload == 0xA5A55A5AU);
  BOOMPI_EXPECT(context, fence.Observe() == 1U);
}

void TestDrainEmptyPartialAndFull(boompi::test::TestContext& context) {
  {
    boompi::audio::TtsIngressQueue queue;
    const auto result = boompi::audio::DrainPublishedTtsFrames(queue);
    BOOMPI_EXPECT(context, result.discarded_frames == 0U);
    BOOMPI_EXPECT(context, result.discarded_source_samples == 0U);
    BOOMPI_EXPECT(context, !result.reached_iteration_bound);
  }

  {
    boompi::audio::TtsIngressQueue queue;
    constexpr std::array<std::uint16_t, 3U> kSampleCounts{{480U, 240U, 1U}};
    for (std::uint32_t index = 0U; index < kSampleCounts.size(); ++index) {
      auto write = queue.TryAcquireWrite();
      BOOMPI_EXPECT(context, static_cast<bool>(write));
      if (!write) {
        return;
      }
      FillTtsFrame(write.get(), index, kSampleCounts[index]);
      BOOMPI_EXPECT(context, write.Publish());
    }
    const auto result = boompi::audio::DrainPublishedTtsFrames(queue);
    BOOMPI_EXPECT(context, result.discarded_frames == 3U);
    BOOMPI_EXPECT(context, result.discarded_source_samples == 721U);
    BOOMPI_EXPECT(context, !result.reached_iteration_bound);
    BOOMPI_EXPECT(context, queue.size_approx() == 0U);
  }

  {
    boompi::audio::TtsIngressQueue queue;
    for (std::uint32_t index = 0U;
         index < boompi::audio::kTtsIngressQueueCapacity; ++index) {
      auto write = queue.TryAcquireWrite();
      BOOMPI_EXPECT(context, static_cast<bool>(write));
      if (!write) {
        return;
      }
      FillTtsFrame(write.get(), index);
      BOOMPI_EXPECT(context, write.Publish());
    }
    BOOMPI_EXPECT(context, !queue.TryAcquireWrite());
    const auto result = boompi::audio::DrainPublishedTtsFrames(queue);
    BOOMPI_EXPECT(context, result.discarded_frames == 64U);
    BOOMPI_EXPECT(context, result.discarded_source_samples == 64U * 480U);
    BOOMPI_EXPECT(context, result.reached_iteration_bound);
    BOOMPI_EXPECT(context, queue.size_approx() == 0U);
  }
}

void TestHeldProducerLeaseAndLatePublish(
    boompi::test::TestContext& context) {
  boompi::audio::TtsIngressQueue queue;
  for (std::uint32_t index = 0U; index < 2U; ++index) {
    auto write = queue.TryAcquireWrite();
    BOOMPI_EXPECT(context, static_cast<bool>(write));
    if (!write) {
      return;
    }
    FillTtsFrame(write.get(), index);
    BOOMPI_EXPECT(context, write.Publish());
  }

  auto held_write = queue.TryAcquireWrite();
  BOOMPI_EXPECT(context, static_cast<bool>(held_write));
  if (!held_write) {
    return;
  }
  FillTtsFrame(held_write.get(), 2U, 123U);
  BOOMPI_EXPECT(context, queue.size_approx() == 2U);
  const auto first_drain = boompi::audio::DrainPublishedTtsFrames(queue);
  BOOMPI_EXPECT(context, first_drain.discarded_frames == 2U);
  BOOMPI_EXPECT(context, first_drain.discarded_source_samples == 960U);
  BOOMPI_EXPECT(context, !first_drain.reached_iteration_bound);
  BOOMPI_EXPECT(context, queue.size_approx() == 0U);
  BOOMPI_EXPECT(context, !queue.TryAcquireRead());

  BOOMPI_EXPECT(context, held_write.Publish());
  BOOMPI_EXPECT(context, queue.size_approx() == 1U);
  const auto late_drain = boompi::audio::DrainPublishedTtsFrames(queue);
  BOOMPI_EXPECT(context, late_drain.discarded_frames == 1U);
  BOOMPI_EXPECT(context, late_drain.discarded_source_samples == 123U);
  BOOMPI_EXPECT(context, !late_drain.reached_iteration_bound);
  BOOMPI_EXPECT(context, queue.size_approx() == 0U);
}

void TestAcceptedRenderQueueBoundariesAndFifo(
    boompi::test::TestContext& context) {
  {
    boompi::audio::AcceptedRenderQueue queue;
    BOOMPI_EXPECT(context, !queue.TryAcquireRead());

    auto write = queue.TryAcquireWrite();
    BOOMPI_EXPECT(context, static_cast<bool>(write));
    if (!write) {
      return;
    }
    FillAcceptedRenderChunk(write.get(), 1U, 960U, 63U, true, true);
    write->write_completed_timestamp_us = 0U;
    BOOMPI_EXPECT(context, !write.Publish());
    BOOMPI_EXPECT(context, queue.size_approx() == 0U);
    write->write_completed_timestamp_us =
        write->metadata.monotonic_timestamp_us + 1000U;
    BOOMPI_EXPECT(context, write.Publish());
    BOOMPI_EXPECT(context, queue.size_approx() == 1U);

    auto read = queue.TryAcquireRead();
    BOOMPI_EXPECT(context, static_cast<bool>(read));
    if (!read) {
      return;
    }
    BOOMPI_EXPECT(context, read->HasValidLength());
    BOOMPI_EXPECT(context, read->accepted_chunk_sequence == 1U);
    BOOMPI_EXPECT(context, read->source_end_of_stream);
    BOOMPI_EXPECT(context, read.Release());

    write = queue.TryAcquireWrite();
    BOOMPI_EXPECT(context, static_cast<bool>(write));
    if (!write) {
      return;
    }
    FillAcceptedRenderChunk(write.get(), 2U, 0U, 1U, true, false);
    BOOMPI_EXPECT(context, write.Cancel());
    BOOMPI_EXPECT(context, queue.size_approx() == 0U);
    BOOMPI_EXPECT(context, !queue.TryAcquireRead());
  }

  {
    boompi::audio::AcceptedRenderQueue queue;
    for (std::uint64_t sequence = 1U;
         sequence <= boompi::audio::kAcceptedRenderQueueCapacity;
         ++sequence) {
      auto write = queue.TryAcquireWrite();
      BOOMPI_EXPECT(context, static_cast<bool>(write));
      if (!write) {
        return;
      }
      FillAcceptedRenderChunk(write.get(), sequence, 0U, 1U, true, false);
      BOOMPI_EXPECT(context, write.Publish());
    }
    BOOMPI_EXPECT(
        context,
        queue.size_approx() ==
            boompi::audio::kAcceptedRenderQueueCapacity);
    BOOMPI_EXPECT(context, !queue.TryAcquireWrite());

    constexpr std::uint64_t kFirstReadCount = 32U;
    for (std::uint64_t sequence = 1U; sequence <= kFirstReadCount;
         ++sequence) {
      auto read = queue.TryAcquireRead();
      BOOMPI_EXPECT(context, static_cast<bool>(read));
      if (!read) {
        return;
      }
      BOOMPI_EXPECT(context, read->accepted_chunk_sequence == sequence);
      BOOMPI_EXPECT(context, read->HasValidLength());
      BOOMPI_EXPECT(context, read.Release());
    }
    BOOMPI_EXPECT(context, queue.size_approx() == 32U);

    const std::uint64_t first_wrapped_sequence =
        boompi::audio::kAcceptedRenderQueueCapacity + 1U;
    const std::uint64_t last_wrapped_sequence =
        boompi::audio::kAcceptedRenderQueueCapacity + kFirstReadCount;
    for (std::uint64_t sequence = first_wrapped_sequence;
         sequence <= last_wrapped_sequence; ++sequence) {
      auto write = queue.TryAcquireWrite();
      BOOMPI_EXPECT(context, static_cast<bool>(write));
      if (!write) {
        return;
      }
      FillAcceptedRenderChunk(write.get(), sequence, 0U, 1U, true, false);
      BOOMPI_EXPECT(context, write.Publish());
    }
    BOOMPI_EXPECT(
        context,
        queue.size_approx() ==
            boompi::audio::kAcceptedRenderQueueCapacity);
    BOOMPI_EXPECT(context, !queue.TryAcquireWrite());

    for (std::uint64_t sequence = kFirstReadCount + 1U;
         sequence <= last_wrapped_sequence; ++sequence) {
      auto read = queue.TryAcquireRead();
      BOOMPI_EXPECT(context, static_cast<bool>(read));
      if (!read) {
        return;
      }
      BOOMPI_EXPECT(context, read->accepted_chunk_sequence == sequence);
      BOOMPI_EXPECT(context, read->accepted_samples == 1U);
      BOOMPI_EXPECT(context, read->HasValidLength());
      BOOMPI_EXPECT(context, read.Release());
    }
    BOOMPI_EXPECT(context, queue.size_approx() == 0U);
    BOOMPI_EXPECT(context, !queue.TryAcquireRead());
  }
}

void TestRenderReferenceQueueCapacity(
    boompi::test::TestContext& context) {
  boompi::audio::RenderReferenceQueue queue;
  for (std::uint32_t index = 0U;
       index < boompi::audio::kRenderReferenceQueueCapacity; ++index) {
    auto write = queue.TryAcquireWrite();
    BOOMPI_EXPECT(context, static_cast<bool>(write));
    if (!write) {
      return;
    }
    *write.get() = MakeRenderReferenceFrame(2U, index);
    BOOMPI_EXPECT(context, write.Publish());
  }
  BOOMPI_EXPECT(context, queue.size_approx() == 16U);
  BOOMPI_EXPECT(context, !queue.TryAcquireWrite());
  for (std::uint32_t index = 0U;
       index < boompi::audio::kRenderReferenceQueueCapacity; ++index) {
    auto read = queue.TryAcquireRead();
    BOOMPI_EXPECT(context, static_cast<bool>(read));
    if (!read) {
      return;
    }
    BOOMPI_EXPECT(context, read->metadata.sequence == index);
    BOOMPI_EXPECT(context, read.Release());
  }
  BOOMPI_EXPECT(context, !queue.TryAcquireRead());
}

}  // namespace

int main() {
  boompi::test::TestContext context;
  TestTtsFrameContract(context);
  TestResampledPcmFrameContract(context);
  TestPlaybackPcmFrameContract(context);
  TestAcceptedRenderChunkContract(context);
  TestRenderReferenceFrameContract(context);
  TestGenerationValidityAndGate(context);
  TestPlaybackEpochFenceValidation(context);
  TestPlaybackEpochFenceReleaseAcquire(context);
  TestDrainEmptyPartialAndFull(context);
  TestHeldProducerLeaseAndLatePublish(context);
  TestAcceptedRenderQueueBoundariesAndFifo(context);
  TestRenderReferenceQueueCapacity(context);

  if (context.failures() != 0U) {
    std::cerr << context.failures()
              << " playback buffer expectation(s) failed\n";
    return 1;
  }
  std::cout << "boomPI playback buffer tests passed\n";
  return 0;
}
