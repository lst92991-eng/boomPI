#include "boompi/audio/wake_word_engine.h"

#include <type_traits>

namespace boompi::audio {
namespace {

static_assert(std::is_trivial<WakeWordProcessResult>::value,
              "wake-word process result must remain a POD value");
static_assert(std::is_standard_layout<WakeWordProcessResult>::value,
              "wake-word process result must remain standard-layout");
static_assert(std::is_trivial<WakeWordResetResult>::value,
              "wake-word reset result must remain a POD value");
static_assert(std::is_standard_layout<WakeWordResetResult>::value,
              "wake-word reset result must remain standard-layout");

constexpr WakeWordProcessResult kNoEventResult{
    WakeWordDecision::kNoEvent, WakeWordError::kNone, 0U, 0U, false};
constexpr WakeWordProcessResult kDetectionWithoutScore{
    WakeWordDecision::kDetected, WakeWordError::kNone, 1U, 0U, false};
constexpr WakeWordProcessResult kUnavailableResult{
    WakeWordDecision::kBackendUnavailable,
    WakeWordError::kBackendUnavailable, 0U, 0U, false};
constexpr WakeWordProcessResult kInconsistentScoreResult{
    WakeWordDecision::kDetected, WakeWordError::kNone, 1U, 1U, false};
constexpr WakeWordProcessResult kInconsistentErrorResult{
    WakeWordDecision::kNoEvent, WakeWordError::kBackendException,
    0U, 0U, false};

static_assert(kNoEventResult.valid() && kNoEventResult.produced() &&
                  !kNoEventResult.detected(),
              "a no-event classification is produced but not detected");
static_assert(kDetectionWithoutScore.valid() &&
                  kDetectionWithoutScore.produced() &&
                  kDetectionWithoutScore.detected(),
              "Snowboy detections remain valid without a confidence score");
static_assert(kUnavailableResult.valid() &&
                  !kUnavailableResult.produced() &&
                  !kUnavailableResult.detected(),
              "an unavailable backend must never claim a classification");
static_assert(!kInconsistentScoreResult.valid(),
              "an unavailable score must be exactly zero");
static_assert(!kInconsistentErrorResult.valid(),
              "successful decisions must not carry backend errors");

bool HasExactMono16kContract(const Mono16kFrame& frame) noexcept {
  return frame.HasValidLength() && frame.format.sample_rate_hz == 16000U &&
         frame.format.frame_duration_ms == 20U &&
         frame.format.channels == 1U &&
         frame.format.sample_format == SampleFormat::kPcmS16Le &&
         frame.samples_per_channel == 320U && frame.metadata.epoch != 0U &&
         frame.metadata.stream_id != 0U;
}

}  // namespace

WakeWordResetResult UnavailableWakeWordEngine::Reset(
    const WakeWordResetReason reason) noexcept {
  (void)reason;
  return {WakeWordError::kBackendUnavailable, false};
}

WakeWordProcessResult UnavailableWakeWordEngine::Process(
    const Mono16kFrame& frame) noexcept {
  if (!HasExactMono16kContract(frame)) {
    return {WakeWordDecision::kInvalidFrame,
            WakeWordError::kInvalidFormat, 0U, 0U, false};
  }

  return {WakeWordDecision::kBackendUnavailable,
          WakeWordError::kBackendUnavailable, 0U, 0U, false};
}

}  // namespace boompi::audio
