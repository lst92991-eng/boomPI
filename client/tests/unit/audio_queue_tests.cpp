#include <atomic>
#include <cstdint>
#include <iostream>
#include <limits>
#include <thread>
#include <utility>

#include "boompi/audio/audio_frame.h"
#include "boompi/audio/frame_continuity_gate.h"
#include "boompi/audio/spsc_audio_frame_queue.h"
#include "boompi/test/test_context.h"

#define BOOMPI_EXPECT(context, expression) \
  (context).Expect((expression), #expression, __FILE__, __LINE__)

namespace {

void TestFrameLength(boompi::test::TestContext& context) {
  boompi::audio::CaptureFrame capture{};
  capture.format = {48000U, 20U, 4U,
                    boompi::audio::SampleFormat::kPcmS16Le};
  capture.samples_per_channel = 960U;
  BOOMPI_EXPECT(context, capture.HasValidLength());
  BOOMPI_EXPECT(context, capture.interleaved_samples() == 3840U);
  BOOMPI_EXPECT(context, capture.sample_capacity() == 3840U);

  capture.samples_per_channel = 959U;
  BOOMPI_EXPECT(context, !capture.HasValidLength());
  capture.samples_per_channel = 961U;
  BOOMPI_EXPECT(context, !capture.HasValidLength());
  BOOMPI_EXPECT(context, capture.interleaved_samples() == 0U);

  capture.samples_per_channel = 960U;
  capture.format.frame_duration_ms = 10U;
  BOOMPI_EXPECT(context, !capture.HasValidLength());
  capture.format = {24000U, 40U, 4U,
                    boompi::audio::SampleFormat::kPcmS16Le};
  BOOMPI_EXPECT(context, !capture.HasValidLength());

  boompi::audio::Mono16kFrame mono{};
  mono.format = {16000U, 20U, 1U,
                 boompi::audio::SampleFormat::kPcmS16Le};
  mono.samples_per_channel = 320U;
  mono.samples[0] = 123;
  BOOMPI_EXPECT(context, mono.HasValidLength());
  mono.format = {8000U, 40U, 1U,
                 boompi::audio::SampleFormat::kPcmS16Le};
  BOOMPI_EXPECT(context, !mono.HasValidLength());
  mono.format = {16000U, 20U, 1U,
                 boompi::audio::SampleFormat::kPcmS16Le};
  BOOMPI_EXPECT(context, mono.interleaved_samples() == 320U);
  mono.ResetHeader();
  BOOMPI_EXPECT(context, !mono.HasValidLength());
  BOOMPI_EXPECT(context, mono.samples[0] == 123);
}

void FillMonoFrame(boompi::audio::Mono16kFrame* const frame,
                   const std::uint32_t sequence) {
  frame->format = {16000U, 20U, 1U,
                   boompi::audio::SampleFormat::kPcmS16Le};
  frame->metadata.sequence = sequence;
  frame->metadata.monotonic_timestamp_us =
      static_cast<std::uint64_t>(sequence) + 1U;
  frame->metadata.stream_id = 11U;
  frame->metadata.epoch = 7U;
  frame->samples_per_channel = 320U;
  for (std::size_t index = 0U; index < frame->samples.size(); ++index) {
    frame->samples[index] =
        static_cast<std::int16_t>((sequence + index) & 0x7FFFU);
  }
}

void TestFifoFullAndReuse(boompi::test::TestContext& context) {
  boompi::audio::SpscAudioFrameQueue<boompi::audio::Mono16kFrame, 4U> queue;
  const boompi::audio::Mono16kFrame* first_slot = nullptr;

  for (std::uint32_t sequence = 0U; sequence < 4U; ++sequence) {
    auto write = queue.TryAcquireWrite();
    BOOMPI_EXPECT(context, static_cast<bool>(write));
    if (!write) {
      return;
    }
    if (sequence == 0U) {
      first_slot = write.get();
    }
    FillMonoFrame(write.get(), sequence);
    BOOMPI_EXPECT(context, write.Publish());
  }

  BOOMPI_EXPECT(context, queue.size_approx() == 4U);
  BOOMPI_EXPECT(context, !queue.TryAcquireWrite());

  auto first_read = queue.TryAcquireRead();
  BOOMPI_EXPECT(context, static_cast<bool>(first_read));
  if (!first_read) {
    return;
  }
  BOOMPI_EXPECT(context, first_read->metadata.sequence == 0U);
  BOOMPI_EXPECT(context, !queue.TryAcquireRead());
  BOOMPI_EXPECT(context, first_read.Release());

  auto reused = queue.TryAcquireWrite();
  BOOMPI_EXPECT(context, static_cast<bool>(reused));
  if (!reused) {
    return;
  }
  BOOMPI_EXPECT(context, reused.get() == first_slot);
  FillMonoFrame(reused.get(), 4U);
  BOOMPI_EXPECT(context, reused.Publish());

  for (std::uint32_t sequence = 1U; sequence <= 4U; ++sequence) {
    auto read = queue.TryAcquireRead();
    BOOMPI_EXPECT(context, static_cast<bool>(read));
    if (!read) {
      return;
    }
    BOOMPI_EXPECT(context, read->metadata.sequence == sequence);
    BOOMPI_EXPECT(context, read.Release());
  }
  BOOMPI_EXPECT(context, !queue.TryAcquireRead());
}

void TestLeaseOwnership(boompi::test::TestContext& context) {
  boompi::audio::SpscAudioFrameQueue<boompi::audio::Mono16kFrame, 2U> queue;

  const boompi::audio::Mono16kFrame* abandoned_slot = nullptr;
  {
    auto abandoned = queue.TryAcquireWrite();
    BOOMPI_EXPECT(context, static_cast<bool>(abandoned));
    if (!abandoned) {
      return;
    }
    abandoned_slot = abandoned.get();
    FillMonoFrame(abandoned.get(), 10U);
  }
  BOOMPI_EXPECT(context, !queue.TryAcquireRead());

  auto original_write = queue.TryAcquireWrite();
  BOOMPI_EXPECT(context, static_cast<bool>(original_write));
  if (!original_write) {
    return;
  }
  BOOMPI_EXPECT(context, original_write.get() == abandoned_slot);
  FillMonoFrame(original_write.get(), 11U);
  auto moved_write = std::move(original_write);
  BOOMPI_EXPECT(context, !original_write);
  BOOMPI_EXPECT(context, static_cast<bool>(moved_write));
  BOOMPI_EXPECT(context, moved_write.Publish());
  BOOMPI_EXPECT(context, !moved_write.Publish());

  auto original_read = queue.TryAcquireRead();
  BOOMPI_EXPECT(context, static_cast<bool>(original_read));
  if (!original_read) {
    return;
  }
  auto moved_read = std::move(original_read);
  BOOMPI_EXPECT(context, !original_read);
  BOOMPI_EXPECT(context, moved_read->metadata.sequence == 11U);
  BOOMPI_EXPECT(context, moved_read.Release());
  BOOMPI_EXPECT(context, !moved_read.Release());

  {
    auto write = queue.TryAcquireWrite();
    BOOMPI_EXPECT(context, static_cast<bool>(write));
    if (!write) {
      return;
    }
    FillMonoFrame(write.get(), 12U);
    BOOMPI_EXPECT(context, write.Publish());
    auto read = queue.TryAcquireRead();
    BOOMPI_EXPECT(context, static_cast<bool>(read));
  }
  BOOMPI_EXPECT(context, !queue.TryAcquireRead());
  BOOMPI_EXPECT(context, queue.size_approx() == 0U);
}

void TestInvalidFrameCannotBePublished(boompi::test::TestContext& context) {
  boompi::audio::SpscAudioFrameQueue<boompi::audio::Mono16kFrame, 2U> queue;
  auto write = queue.TryAcquireWrite();
  BOOMPI_EXPECT(context, static_cast<bool>(write));
  if (!write) {
    return;
  }

  write->format = {16000U, 20U, 1U,
                   boompi::audio::SampleFormat::kPcmS16Le};
  write->samples_per_channel = 319U;
  write->metadata.epoch = 1U;
  write->metadata.stream_id = 1U;
  BOOMPI_EXPECT(context, !write.Publish());
  BOOMPI_EXPECT(context, !queue.TryAcquireWrite());

  write->samples_per_channel = 320U;
  write->metadata.epoch = 0U;
  write->metadata.stream_id = 1U;
  BOOMPI_EXPECT(context, !write.Publish());
  write->metadata.epoch = 1U;
  write->metadata.stream_id = 0U;
  BOOMPI_EXPECT(context, !write.Publish());

  boompi::audio::AudioFrameSequencer uninitialized_sequencer;
  write->metadata = uninitialized_sequencer.NextPublished(1000U, 0U);
  BOOMPI_EXPECT(context, !write.Publish());

  write->metadata.epoch = 1U;
  write->metadata.stream_id = 1U;
  BOOMPI_EXPECT(context, write.Publish());
  BOOMPI_EXPECT(context, static_cast<bool>(queue.TryAcquireRead()));
}

void TestMetadataAndPcm(boompi::test::TestContext& context) {
  boompi::audio::SpscAudioFrameQueue<boompi::audio::Mono16kFrame, 1U> queue;
  auto write = queue.TryAcquireWrite();
  BOOMPI_EXPECT(context, static_cast<bool>(write));
  if (!write) {
    return;
  }

  FillMonoFrame(write.get(), 42U);
  write->metadata.monotonic_timestamp_us = 1234567U;
  write->metadata.stream_id = 9U;
  write->metadata.turn_id = 7U;
  write->metadata.epoch = 3U;
  write->metadata.discontinuity = true;
  write->samples[0] = -1234;
  write->samples[319] = 2345;
  BOOMPI_EXPECT(context, write.Publish());

  auto read = queue.TryAcquireRead();
  BOOMPI_EXPECT(context, static_cast<bool>(read));
  if (!read) {
    return;
  }
  BOOMPI_EXPECT(context, read->format.sample_rate_hz == 16000U);
  BOOMPI_EXPECT(context, read->format.frame_duration_ms == 20U);
  BOOMPI_EXPECT(context, read->format.channels == 1U);
  BOOMPI_EXPECT(context, read->metadata.monotonic_timestamp_us == 1234567U);
  BOOMPI_EXPECT(context, read->metadata.sequence == 42U);
  BOOMPI_EXPECT(context, read->metadata.stream_id == 9U);
  BOOMPI_EXPECT(context, read->metadata.turn_id == 7U);
  BOOMPI_EXPECT(context, read->metadata.epoch == 3U);
  BOOMPI_EXPECT(context, read->metadata.discontinuity);
  BOOMPI_EXPECT(context, read->samples[0] == -1234);
  BOOMPI_EXPECT(context, read->samples[319] == 2345);
}

void TestSequencer(boompi::test::TestContext& context) {
  boompi::audio::AudioFrameSequencer sequencer;
  BOOMPI_EXPECT(context, !sequencer.initialized());
  BOOMPI_EXPECT(context, !sequencer.Reset(0U, 1U));
  BOOMPI_EXPECT(context, !sequencer.Reset(1U, 0U));
  BOOMPI_EXPECT(context, sequencer.Reset(2U, 5U));

  auto metadata = sequencer.NextPublished(1000U, 0U);
  BOOMPI_EXPECT(context, metadata.sequence == 0U);
  BOOMPI_EXPECT(context, metadata.epoch == 2U);
  BOOMPI_EXPECT(context, metadata.stream_id == 5U);
  BOOMPI_EXPECT(context, metadata.turn_id == 0U);
  BOOMPI_EXPECT(context, !metadata.discontinuity);

  sequencer.AccountDroppedFrame();
  metadata = sequencer.NextPublished(2000U, 17U);
  BOOMPI_EXPECT(context, metadata.sequence == 2U);
  BOOMPI_EXPECT(context, metadata.turn_id == 17U);
  BOOMPI_EXPECT(context, metadata.discontinuity);

  metadata = sequencer.NextPublished(3000U, 17U);
  BOOMPI_EXPECT(context, metadata.sequence == 3U);
  BOOMPI_EXPECT(context, !metadata.discontinuity);

  sequencer.MarkDiscontinuity();
  metadata = sequencer.NextPublished(4000U, 17U);
  BOOMPI_EXPECT(context, metadata.sequence == 4U);
  BOOMPI_EXPECT(context, metadata.discontinuity);

  BOOMPI_EXPECT(context, !sequencer.Reset(0U, 9U));
  metadata = sequencer.NextPublished(5000U, 17U);
  BOOMPI_EXPECT(context, metadata.sequence == 5U);
  BOOMPI_EXPECT(context, metadata.epoch == 2U);
  BOOMPI_EXPECT(context, metadata.stream_id == 5U);

  BOOMPI_EXPECT(context, !sequencer.Reset(2U, 5U, 99U));

  BOOMPI_EXPECT(
      context,
      sequencer.Reset(3U, 8U, std::numeric_limits<std::uint32_t>::max()));
  metadata = sequencer.NextPublished(6000U, 0U);
  BOOMPI_EXPECT(
      context,
      metadata.sequence == std::numeric_limits<std::uint32_t>::max());
  metadata = sequencer.NextPublished(7000U, 0U);
  BOOMPI_EXPECT(context, metadata.sequence == 0U);
  BOOMPI_EXPECT(context, metadata.discontinuity);
}

void TestDropAccountingReachesContinuityGate(
    boompi::test::TestContext& context) {
  boompi::audio::SpscAudioFrameQueue<boompi::audio::Mono16kFrame, 1U> queue;
  boompi::audio::AudioFrameSequencer sequencer;
  boompi::audio::FrameContinuityGate gate;
  BOOMPI_EXPECT(context, sequencer.Reset(3U, 9U));
  BOOMPI_EXPECT(context, gate.Arm(3U, 9U).ok());

  auto publish = [&queue, &sequencer](const std::uint64_t timestamp_us) {
    auto write = queue.TryAcquireWrite();
    if (!write) {
      sequencer.AccountDroppedFrame();
      return false;
    }
    FillMonoFrame(write.get(), 0U);
    write->metadata = sequencer.NextPublished(timestamp_us, 0U);
    return write.Publish();
  };

  BOOMPI_EXPECT(context, publish(1000U));
  BOOMPI_EXPECT(context, !publish(2000U));
  auto first = queue.TryAcquireRead();
  BOOMPI_EXPECT(context, static_cast<bool>(first));
  if (!first) {
    return;
  }
  BOOMPI_EXPECT(
      context,
      gate.CheckAndAdvance(first->metadata) ==
          boompi::audio::FrameContinuityResult::kAcceptedFirst);
  first.Release();

  BOOMPI_EXPECT(context, publish(3000U));
  auto after_drop = queue.TryAcquireRead();
  BOOMPI_EXPECT(context, static_cast<bool>(after_drop));
  if (!after_drop) {
    return;
  }
  BOOMPI_EXPECT(context, after_drop->metadata.sequence == 2U);
  BOOMPI_EXPECT(context, after_drop->metadata.discontinuity);
  BOOMPI_EXPECT(
      context,
      gate.CheckAndAdvance(after_drop->metadata) ==
          boompi::audio::FrameContinuityResult::kDiscontinuity);
}

void TestCancelledPeriodAccountingReachesContinuityGate(
    boompi::test::TestContext& context) {
  boompi::audio::SpscAudioFrameQueue<boompi::audio::Mono16kFrame, 2U> queue;
  boompi::audio::AudioFrameSequencer sequencer;
  boompi::audio::FrameContinuityGate gate;
  BOOMPI_EXPECT(context, sequencer.Reset(4U, 10U));
  BOOMPI_EXPECT(context, gate.Arm(4U, 10U).ok());

  auto first_write = queue.TryAcquireWrite();
  BOOMPI_EXPECT(context, static_cast<bool>(first_write));
  if (!first_write) {
    return;
  }
  FillMonoFrame(first_write.get(), 0U);
  first_write->metadata = sequencer.NextPublished(1000U, 0U);
  BOOMPI_EXPECT(context, first_write.Publish());
  auto first_read = queue.TryAcquireRead();
  BOOMPI_EXPECT(context, static_cast<bool>(first_read));
  if (!first_read) {
    return;
  }
  BOOMPI_EXPECT(
      context,
      gate.CheckAndAdvance(first_read->metadata) ==
          boompi::audio::FrameContinuityResult::kAcceptedFirst);
  first_read.Release();

  auto partial_period = queue.TryAcquireWrite();
  BOOMPI_EXPECT(context, static_cast<bool>(partial_period));
  if (!partial_period) {
    return;
  }
  FillMonoFrame(partial_period.get(), 1U);
  BOOMPI_EXPECT(context, partial_period.Cancel());
  sequencer.AccountDroppedFrame();

  auto recovered_write = queue.TryAcquireWrite();
  BOOMPI_EXPECT(context, static_cast<bool>(recovered_write));
  if (!recovered_write) {
    return;
  }
  FillMonoFrame(recovered_write.get(), 2U);
  recovered_write->metadata = sequencer.NextPublished(3000U, 0U);
  BOOMPI_EXPECT(context, recovered_write.Publish());
  auto recovered_read = queue.TryAcquireRead();
  BOOMPI_EXPECT(context, static_cast<bool>(recovered_read));
  if (!recovered_read) {
    return;
  }
  BOOMPI_EXPECT(context, recovered_read->metadata.sequence == 2U);
  BOOMPI_EXPECT(context, recovered_read->metadata.discontinuity);
  BOOMPI_EXPECT(
      context,
      gate.CheckAndAdvance(recovered_read->metadata) ==
          boompi::audio::FrameContinuityResult::kDiscontinuity);
}

void TestConcurrentFifo(boompi::test::TestContext& context) {
  constexpr std::uint32_t kFrameCount = 100000U;
  boompi::audio::SpscAudioFrameQueue<boompi::audio::Mono16kFrame, 64U> queue;
  std::atomic<bool> failed{false};

  std::thread producer([&queue, &failed]() {
    for (std::uint32_t sequence = 0U; sequence < kFrameCount; ++sequence) {
      for (;;) {
        auto write = queue.TryAcquireWrite();
        if (!write) {
          std::this_thread::yield();
          continue;
        }
        FillMonoFrame(write.get(), sequence);
        write->samples[319] =
            static_cast<std::int16_t>((sequence * 3U) & 0x7FFFU);
        if (!write.Publish()) {
          failed.store(true, std::memory_order_relaxed);
          return;
        }
        break;
      }
    }
  });

  std::thread consumer([&queue, &failed]() {
    for (std::uint32_t expected = 0U; expected < kFrameCount; ++expected) {
      for (;;) {
        auto read = queue.TryAcquireRead();
        if (!read) {
          if (failed.load(std::memory_order_relaxed)) {
            return;
          }
          std::this_thread::yield();
          continue;
        }
        const auto expected_tail =
            static_cast<std::int16_t>((expected * 3U) & 0x7FFFU);
        if (read->metadata.sequence != expected ||
            read->samples[0] !=
                static_cast<std::int16_t>(expected & 0x7FFFU) ||
            read->samples[319] != expected_tail) {
          failed.store(true, std::memory_order_relaxed);
        }
        read.Release();
        break;
      }
    }
  });

  producer.join();
  consumer.join();
  BOOMPI_EXPECT(context, !failed.load(std::memory_order_relaxed));
  BOOMPI_EXPECT(context, queue.size_approx() == 0U);
}

}  // namespace

int main() {
  boompi::test::TestContext context;
  TestFrameLength(context);
  TestFifoFullAndReuse(context);
  TestLeaseOwnership(context);
  TestInvalidFrameCannotBePublished(context);
  TestMetadataAndPcm(context);
  TestSequencer(context);
  TestDropAccountingReachesContinuityGate(context);
  TestCancelledPeriodAccountingReachesContinuityGate(context);
  TestConcurrentFifo(context);

  if (context.failures() != 0U) {
    std::cerr << context.failures()
              << " boomPI audio queue expectation(s) failed\n";
    return 1;
  }
  std::cout << "boomPI audio queue tests passed\n";
  return 0;
}
