#include "boompi/audio/capture_channel_mapper.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace boompi::audio {
namespace {

static_assert(static_cast<std::size_t>(CaptureRole::kReferenceRight) + 1U ==
                  kDspCaptureChannelCount,
              "CaptureRole and capture plane counts must remain aligned");

SampleTransformResult InvalidTransformResult() noexcept {
  return {SampleTransformCode::kInvalidArgument, 0U};
}

}  // namespace

Status CaptureChannelMapper::Create(const ChannelMap& channel_map,
                                    CaptureChannelMapper* const output) {
  if (output == nullptr) {
    return Status::Error(StatusCode::kInvalidArgument,
                         "capture channel mapper output must not be null");
  }

  std::array<bool, kDspCaptureChannelCount> input_slot_seen{};
  for (const ChannelRoute& route : channel_map.routes) {
    const auto input_slot = static_cast<std::size_t>(route.input_slot);
    if (input_slot >= kDspCaptureChannelCount) {
      return Status::Error(StatusCode::kInvalidArgument,
                           "capture input slot must be between 0 and 3");
    }
    if (input_slot_seen[input_slot]) {
      return Status::Error(StatusCode::kInvalidArgument,
                           "capture input slots must not be repeated");
    }
    if (route.polarity != 1 && route.polarity != -1) {
      return Status::Error(StatusCode::kInvalidArgument,
                           "capture channel polarity must be +1 or -1");
    }
    input_slot_seen[input_slot] = true;
  }

  output->channel_map_ = channel_map;
  output->configured_ = true;
  return Status::Ok();
}

SampleTransformResult CaptureChannelMapper::Process(
    const CaptureFrame& input, CapturePlanes48k* const output) const noexcept {
  if (!configured_ || output == nullptr || !input.HasValidLength() ||
      input.format.sample_rate_hz != 48000U ||
      input.format.frame_duration_ms != 20U || input.format.channels != 4U ||
      input.samples_per_channel != kSamplesPer48k20ms) {
    return InvalidTransformResult();
  }

  std::uint32_t saturated_samples = 0U;
  for (std::size_t role_index = 0U;
       role_index < kDspCaptureChannelCount; ++role_index) {
    const ChannelRoute& route = channel_map_.routes[role_index];
    const auto input_slot = static_cast<std::size_t>(route.input_slot);
    const auto polarity = static_cast<std::int32_t>(route.polarity);
    CapturePlane48k& output_plane = (*output)[role_index];

    for (std::size_t sample_index = 0U;
         sample_index < kSamplesPer48k20ms; ++sample_index) {
      const std::size_t interleaved_index =
          sample_index * kDspCaptureChannelCount + input_slot;
      const auto transformed =
          static_cast<std::int32_t>(input.samples[interleaved_index]) *
          polarity;
      if (transformed > std::numeric_limits<std::int16_t>::max()) {
        output_plane[sample_index] =
            std::numeric_limits<std::int16_t>::max();
        ++saturated_samples;
      } else if (transformed < std::numeric_limits<std::int16_t>::min()) {
        output_plane[sample_index] =
            std::numeric_limits<std::int16_t>::min();
        ++saturated_samples;
      } else {
        output_plane[sample_index] =
            static_cast<std::int16_t>(transformed);
      }
    }
  }

  return {SampleTransformCode::kOk, saturated_samples};
}

}  // namespace boompi::audio
