#ifndef BOOMPI_AUDIO_WAKE_WORD_ENGINE_H_
#define BOOMPI_AUDIO_WAKE_WORD_ENGINE_H_

#include <cstdint>

#include "boompi/audio/audio_frame.h"

namespace boompi::audio {

enum class WakeWordDecision : std::uint8_t {
  kNoEvent = 0,
  kSilence,
  kDetected,
  kInvalidFrame,
  kBackendUnavailable,
  kBackendError,
  kFaulted,
};

enum class WakeWordError : std::uint8_t {
  kNone = 0,
  kInvalidFormat,
  kNotInitialized,
  kBackendUnavailable,
  kBackendRejectedAudio,
  kBackendException,
  kUnexpectedKeywordIndex,
  kResetFailed,
};

// score_milli is normalized to [0, 1000] only when score_available is true.
// Snowboy does not expose a confidence score, so its adapter must return
// score_available=false and score_milli=0. Its -2 result maps to kSilence for
// diagnostics only and must never be used as the product VAD decision.
struct WakeWordProcessResult final {
  WakeWordDecision decision;
  WakeWordError error;
  // Snowboy-compatible one-based keyword index; zero means no detection.
  std::uint16_t keyword_index;
  std::uint16_t score_milli;
  bool score_available;

  // produced means the backend successfully classified this frame. It does
  // not imply that a wake word was detected.
  constexpr bool produced() const noexcept {
    return valid() &&
           (decision == WakeWordDecision::kNoEvent ||
            decision == WakeWordDecision::kSilence ||
            decision == WakeWordDecision::kDetected);
  }

  constexpr bool detected() const noexcept {
    return valid() && decision == WakeWordDecision::kDetected;
  }

  constexpr bool valid() const noexcept {
    if (score_milli > 1000U ||
        (!score_available && score_milli != 0U)) {
      return false;
    }

    const bool has_empty_payload =
        keyword_index == 0U && score_milli == 0U && !score_available;
    switch (decision) {
      case WakeWordDecision::kNoEvent:
      case WakeWordDecision::kSilence:
        return error == WakeWordError::kNone && has_empty_payload;
      case WakeWordDecision::kDetected:
        return error == WakeWordError::kNone && keyword_index != 0U;
      case WakeWordDecision::kInvalidFrame:
        return error == WakeWordError::kInvalidFormat && has_empty_payload;
      case WakeWordDecision::kBackendUnavailable:
        return error == WakeWordError::kBackendUnavailable &&
               has_empty_payload;
      case WakeWordDecision::kBackendError:
      case WakeWordDecision::kFaulted:
        return IsBackendFailure(error) && has_empty_payload;
    }
    return false;
  }

 private:
  static constexpr bool IsBackendFailure(
      const WakeWordError candidate) noexcept {
    return candidate == WakeWordError::kNotInitialized ||
           candidate == WakeWordError::kBackendRejectedAudio ||
           candidate == WakeWordError::kBackendException ||
           candidate == WakeWordError::kUnexpectedKeywordIndex ||
           candidate == WakeWordError::kResetFailed;
  }
};

enum class WakeWordResetReason : std::uint8_t {
  kGenerationActivated = 0,
  kVadSegmentEnded,
  kDiscontinuity,
};

struct WakeWordResetResult final {
  WakeWordError error;
  bool reset;

  constexpr bool valid() const noexcept {
    return reset ? error == WakeWordError::kNone
                 : error != WakeWordError::kNone;
  }
};

// One wake/VAD worker constructs, uses, resets, and destroys an engine. The
// engine has no internal synchronization. Generation identity, frame
// continuity, pre-roll, and product VAD remain responsibilities of that
// worker and its FrameContinuityGate. Process and Reset are noexcept hot-path
// operations and must not allocate, lock, log, or perform file/network I/O.
class WakeWordEngine {
 public:
  WakeWordEngine() noexcept = default;
  WakeWordEngine(const WakeWordEngine&) = delete;
  WakeWordEngine& operator=(const WakeWordEngine&) = delete;
  WakeWordEngine(WakeWordEngine&&) = delete;
  WakeWordEngine& operator=(WakeWordEngine&&) = delete;
  virtual ~WakeWordEngine() noexcept = default;

  virtual WakeWordResetResult Reset(
      WakeWordResetReason reason) noexcept = 0;
  virtual WakeWordProcessResult Process(
      const Mono16kFrame& frame) noexcept = 0;
};

// Fail-closed implementation used when the optional Snowboy build inputs are
// absent. It never reports silence, activity, or a detection and is not a
// fallback wake-word algorithm.
class UnavailableWakeWordEngine final : public WakeWordEngine {
 public:
  WakeWordResetResult Reset(WakeWordResetReason reason) noexcept override;
  WakeWordProcessResult Process(
      const Mono16kFrame& frame) noexcept override;
};

}  // namespace boompi::audio

#endif  // BOOMPI_AUDIO_WAKE_WORD_ENGINE_H_
