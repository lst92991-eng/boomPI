#include "boompi/audio/playback_buffer.h"

#include <type_traits>

namespace boompi::audio {

static_assert(std::is_trivially_copyable<PlaybackDrainResult>::value,
              "playback drain result must remain trivially copyable");
static_assert(std::is_standard_layout<PlaybackDrainResult>::value,
              "playback drain result must remain standard-layout");
static_assert(TtsIngressQueue::capacity() == kTtsIngressQueueCapacity,
              "TTS drain bound must match the ingress queue capacity");

PlaybackDrainResult DrainPublishedTtsFrames(
    TtsIngressQueue& queue) noexcept {
  PlaybackDrainResult result{0U, 0U, false};

  for (std::uint32_t iteration = 0U;
       iteration < kTtsIngressQueueCapacity; ++iteration) {
    auto lease = queue.TryAcquireRead();
    if (!lease) {
      return result;
    }

    ++result.discarded_frames;
    if (lease->HasValidLength()) {
      result.discarded_source_samples += lease->valid_source_samples;
    }
    (void)lease.Release();

    if (result.discarded_frames == kTtsIngressQueueCapacity) {
      result.reached_iteration_bound = true;
    }
  }

  return result;
}

}  // namespace boompi::audio
