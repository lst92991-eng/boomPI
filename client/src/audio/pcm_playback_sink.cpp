#include "boompi/audio/pcm_playback_sink.h"

#include <type_traits>

namespace boompi::audio {
namespace {

static_assert(std::is_trivial<PcmPlaybackWriteResult>::value,
              "PCM playback write result must remain a POD value");
static_assert(std::is_standard_layout<PcmPlaybackWriteResult>::value,
              "PCM playback write result must remain standard-layout");
static_assert(std::is_trivial<PcmPlaybackControlResult>::value,
              "PCM playback control result must remain a POD value");
static_assert(std::is_standard_layout<PcmPlaybackControlResult>::value,
              "PCM playback control result must remain standard-layout");
static_assert(PcmPlaybackWriteResult::kMaximumQueuedSampleFrames ==
                  RenderReferenceFrame48k::kMaximumQueuedSampleFrames,
              "sink and render-reference queue bounds must agree");

constexpr PcmPlaybackWriteResult kUnavailableWriteResult{
    PcmPlaybackWriteCode::kUnavailable, 0U, 0U, 0U, 0U,
    PlaybackTimingSource::kUnset, 0};
constexpr PcmPlaybackControlResult kUnavailableControlResult{
    PcmPlaybackControlCode::kUnavailable, 0};
constexpr PcmPlaybackControlResult kDefaultControlResult{};

static_assert(kUnavailableWriteResult.valid() &&
                  !kUnavailableWriteResult.accepted(),
              "unavailable sink writes must fail closed");
static_assert(kUnavailableControlResult.valid() &&
                  !kUnavailableControlResult.succeeded(),
              "unavailable sink controls must fail closed");
static_assert(!kDefaultControlResult.valid() &&
                  !kDefaultControlResult.succeeded(),
              "value-initialized sink controls must fail closed");

}  // namespace

PcmPlaybackWriteResult
UnavailablePcmPlaybackSink::TryWriteInterleavedS16(
    const std::int16_t* const samples,
    const std::uint16_t frames) noexcept {
  static_cast<void>(samples);
  static_cast<void>(frames);
  return kUnavailableWriteResult;
}

PcmPlaybackControlResult UnavailablePcmPlaybackSink::Drop() noexcept {
  return kUnavailableControlResult;
}

PcmPlaybackControlResult UnavailablePcmPlaybackSink::Prepare() noexcept {
  return kUnavailableControlResult;
}

}  // namespace boompi::audio
