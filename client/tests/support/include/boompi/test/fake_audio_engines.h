#ifndef BOOMPI_TEST_FAKE_AUDIO_ENGINES_H_
#define BOOMPI_TEST_FAKE_AUDIO_ENGINES_H_

#include <cstdint>

#include "boompi/audio/audio_dsp_engine.h"
#include "boompi/audio/wake_word_engine.h"

namespace boompi::test {

// Deterministic test double for orchestration and generation tests. It is not
// a DSP implementation and must never be used as an AEC/NS/BF/AGC fallback.
class FakeAudioDspEngine final : public audio::AudioDspEngine {
 public:
  FakeAudioDspEngine() noexcept = default;

  Status Configure(const audio::AudioDspEngineConfig& config) override;
  Status Arm(std::uint32_t epoch, std::uint32_t stream_id) override;
  void Disarm() noexcept override;
  audio::AudioDspProcessResult Process(
      const audio::DspCaptureFrame16k& input,
      audio::Mono16kFrame* output) noexcept override;

  void FailNextProcess() noexcept { fail_next_process_ = true; }

  std::uint32_t configure_count() const noexcept { return configure_count_; }
  std::uint32_t arm_count() const noexcept { return arm_count_; }
  std::uint32_t backend_process_count() const noexcept {
    return backend_process_count_;
  }
  std::uint32_t reset_count() const noexcept { return reset_count_; }
  std::uint32_t disarm_count() const noexcept { return disarm_count_; }
  std::uint32_t active_epoch() const noexcept { return active_epoch_; }
  std::uint32_t active_stream_id() const noexcept {
    return active_stream_id_;
  }
  bool configured() const noexcept { return configured_; }
  bool armed() const noexcept { return armed_; }
  bool faulted() const noexcept { return faulted_; }

 private:
  std::uint32_t active_epoch_{0U};
  std::uint32_t active_stream_id_{0U};
  std::uint32_t last_epoch_{0U};
  std::uint32_t configure_count_{0U};
  std::uint32_t arm_count_{0U};
  std::uint32_t backend_process_count_{0U};
  std::uint32_t reset_count_{0U};
  std::uint32_t disarm_count_{0U};
  bool configured_{false};
  bool armed_{false};
  bool faulted_{false};
  bool fail_next_process_{false};
};

// Deterministic test double for wake-worker control tests. It does not load
// Snowboy, inspect speech, or implement any wake-word algorithm.
class FakeWakeWordEngine final : public audio::WakeWordEngine {
 public:
  FakeWakeWordEngine() noexcept = default;

  audio::WakeWordResetResult Reset(
      audio::WakeWordResetReason reason) noexcept override;
  audio::WakeWordProcessResult Process(
      const audio::Mono16kFrame& frame) noexcept override;

  // A single fixed slot prevents an unbounded test script from entering a
  // hot-path fake. Invalid results and overwriting a pending result fail.
  bool SetNextResult(const audio::WakeWordProcessResult& result) noexcept;
  bool SetNextResetError(audio::WakeWordError error) noexcept;

  std::uint32_t process_count() const noexcept { return process_count_; }
  std::uint32_t reset_count() const noexcept { return reset_count_; }
  bool has_last_reset_reason() const noexcept {
    return has_last_reset_reason_;
  }
  audio::WakeWordResetReason last_reset_reason() const noexcept {
    return last_reset_reason_;
  }
  bool scripted_result_pending() const noexcept {
    return scripted_result_pending_;
  }
  bool reset_error_pending() const noexcept {
    return reset_error_pending_;
  }

 private:
  audio::WakeWordProcessResult scripted_result_{
      audio::WakeWordDecision::kNoEvent, audio::WakeWordError::kNone,
      0U, 0U, false};
  audio::WakeWordError next_reset_error_{audio::WakeWordError::kNone};
  audio::WakeWordResetReason last_reset_reason_{
      audio::WakeWordResetReason::kGenerationActivated};
  std::uint32_t process_count_{0U};
  std::uint32_t reset_count_{0U};
  bool scripted_result_pending_{false};
  bool reset_error_pending_{false};
  bool has_last_reset_reason_{false};
};

}  // namespace boompi::test

#endif  // BOOMPI_TEST_FAKE_AUDIO_ENGINES_H_
