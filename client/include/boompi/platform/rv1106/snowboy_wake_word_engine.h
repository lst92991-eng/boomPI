#ifndef BOOMPI_PLATFORM_RV1106_SNOWBOY_WAKE_WORD_ENGINE_H_
#define BOOMPI_PLATFORM_RV1106_SNOWBOY_WAKE_WORD_ENGINE_H_

#include <cstdint>
#include <memory>
#include <string>

#include "boompi/audio/wake_word_engine.h"

namespace boompi::platform::rv1106 {

struct SnowboyWakeWordConfig final {
  std::string resource_path;
  std::string model_path;
  std::string sensitivity{"0.5"};
  float audio_gain{1.0F};
  bool apply_frontend{false};
};

enum class SnowboyWakeWordCreateError : std::uint8_t {
  kNone = 0,
  kInvalidConfig,
  kAllocationFailed,
  kBackendException,
  kUnsupportedAudioFormat,
  kInvalidKeywordCount,
};

struct SnowboyWakeWordCreateResult final {
  SnowboyWakeWordCreateError error{SnowboyWakeWordCreateError::kInvalidConfig};
  std::uint32_t sample_rate_hz{0U};
  std::uint16_t channels{0U};
  std::uint16_t bits_per_sample{0U};
  std::uint16_t keyword_count{0U};

  constexpr bool ok() const noexcept {
    return error == SnowboyWakeWordCreateError::kNone;
  }
};

// This adapter is owned and called by one wake/VAD worker. Creation and
// destruction are cold-path operations that may load files and allocate inside
// Snowboy. Process and Reset add no boomPI allocations, locks, logging, or I/O.
//
// The private legacy bridge is the only translation unit compiled with
// _GLIBCXX_USE_CXX11_ABI=0. This public type never exposes Snowboy objects,
// std::string values from the old ABI, exceptions, or RTTI across that bridge.
class SnowboyWakeWordEngine final : public audio::WakeWordEngine {
 public:
  static std::unique_ptr<SnowboyWakeWordEngine> Create(
      const SnowboyWakeWordConfig& config,
      SnowboyWakeWordCreateResult* result) noexcept;

  SnowboyWakeWordEngine(const SnowboyWakeWordEngine&) = delete;
  SnowboyWakeWordEngine& operator=(const SnowboyWakeWordEngine&) = delete;
  SnowboyWakeWordEngine(SnowboyWakeWordEngine&&) = delete;
  SnowboyWakeWordEngine& operator=(SnowboyWakeWordEngine&&) = delete;
  ~SnowboyWakeWordEngine() noexcept override;

  audio::WakeWordResetResult Reset(
      audio::WakeWordResetReason reason) noexcept override;
  audio::WakeWordProcessResult Process(
      const audio::Mono16kFrame& frame) noexcept override;

  std::uint16_t keyword_count() const noexcept;

 private:
  SnowboyWakeWordEngine(void* backend_handle,
                        std::uint16_t keyword_count) noexcept;

  audio::WakeWordProcessResult LatchBackendFailure(
      audio::WakeWordError error) noexcept;

  void* backend_handle_{nullptr};
  std::uint16_t keyword_count_{0U};
  audio::WakeWordError fault_error_{audio::WakeWordError::kNone};
};

}  // namespace boompi::platform::rv1106

#endif  // BOOMPI_PLATFORM_RV1106_SNOWBOY_WAKE_WORD_ENGINE_H_
