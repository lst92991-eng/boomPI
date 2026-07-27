#ifndef BOOMPI_AUDIO_PLAYBACK_BUFFER_H_
#define BOOMPI_AUDIO_PLAYBACK_BUFFER_H_

#include <cstdint>

#include "boompi/audio/playback_frame.h"
#include "boompi/audio/spsc_audio_frame_queue.h"

namespace boompi::audio {

constexpr std::uint32_t kTtsIngressQueueCapacity = 64U;
constexpr std::uint32_t kAcceptedRenderQueueCapacity = 64U;
constexpr std::uint32_t kRenderReferenceQueueCapacity = 16U;

using TtsIngressQueue =
    SpscAudioFrameQueue<TtsPcmFrame24k, kTtsIngressQueueCapacity>;
using AcceptedRenderQueue =
    SpscAudioFrameQueue<AcceptedRenderChunk48k,
                        kAcceptedRenderQueueCapacity>;
using RenderReferenceQueue =
    SpscAudioFrameQueue<RenderReferenceFrame48k,
                        kRenderReferenceQueueCapacity>;

struct PlaybackDrainResult final {
  std::uint32_t discarded_frames;
  std::uint32_t discarded_source_samples;
  bool reached_iteration_bound;
};

// Consumer-only cancellation helper. The playback thread must not hold a
// ConsumerLease when calling this function. It performs at most one bounded
// pass over the queue's fixed capacity. Reaching the bound does not prove the
// queue is empty. Returning below the bound only means that no published frame
// was visible at that instant; a producer-held lease may still publish later.
PlaybackDrainResult DrainPublishedTtsFrames(TtsIngressQueue& queue) noexcept;

}  // namespace boompi::audio

#endif  // BOOMPI_AUDIO_PLAYBACK_BUFFER_H_
