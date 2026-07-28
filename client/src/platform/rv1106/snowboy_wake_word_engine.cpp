#include "boompi/platform/rv1106/snowboy_wake_word_engine.h"

#include <cmath>
#include <cstdint>
#include <limits>
#include <new>
#include <utility>

#include "snowboy_legacy_bridge.h"

namespace boompi::platform::rv1106 {
namespace {

using audio::Mono16kFrame;
using audio::SampleFormat;
using audio::WakeWordDecision;
using audio::WakeWordError;
using audio::WakeWordProcessResult;

constexpr WakeWordProcessResult kInvalidFrame{
    WakeWordDecision::kInvalidFrame, WakeWordError::kInvalidFormat,
    0U, 0U, false};
constexpr WakeWordProcessResult kNoEvent{
    WakeWordDecision::kNoEvent, WakeWordError::kNone,
    0U, 0U, false};
constexpr WakeWordProcessResult kSilence{
    WakeWordDecision::kSilence, WakeWordError::kNone,
    0U, 0U, false};

bool HasExactMono16kContract(const Mono16kFrame& frame) noexcept {
  return frame.HasValidLength() && frame.format.sample_rate_hz == 16000U &&
         frame.format.frame_duration_ms == 20U &&
         frame.format.channels == 1U &&
         frame.format.sample_format == SampleFormat::kPcmS16Le &&
         frame.samples_per_channel == 320U &&
         frame.metadata.epoch != 0U && frame.metadata.stream_id != 0U;
}

SnowboyWakeWordCreateError MapCreateStatus(
    const BoompiSnowboyLegacyStatus status) noexcept {
  switch (status) {
    case BOOMPI_SNOWBOY_LEGACY_OK:
      return SnowboyWakeWordCreateError::kNone;
    case BOOMPI_SNOWBOY_LEGACY_INVALID_ARGUMENT:
    case BOOMPI_SNOWBOY_LEGACY_BACKEND_REJECTED:
      return SnowboyWakeWordCreateError::kInvalidConfig;
    case BOOMPI_SNOWBOY_LEGACY_ALLOCATION_FAILED:
      return SnowboyWakeWordCreateError::kAllocationFailed;
    case BOOMPI_SNOWBOY_LEGACY_BACKEND_EXCEPTION:
      return SnowboyWakeWordCreateError::kBackendException;
  }
  return SnowboyWakeWordCreateError::kBackendException;
}

}  // namespace

std::unique_ptr<SnowboyWakeWordEngine> SnowboyWakeWordEngine::Create(
    const SnowboyWakeWordConfig& config,
    SnowboyWakeWordCreateResult* const result) noexcept {
  SnowboyWakeWordCreateResult local_result{};
  if (config.resource_path.empty() || config.model_path.empty() ||
      config.sensitivity.empty() || !std::isfinite(config.audio_gain) ||
      config.audio_gain <= 0.0F) {
    if (result != nullptr) {
      *result = local_result;
    }
    return nullptr;
  }

  BoompiSnowboyLegacyHandle* handle = nullptr;
  BoompiSnowboyLegacyInfo info{};
  const BoompiSnowboyLegacyStatus status = boompi_snowboy_legacy_create(
      config.resource_path.c_str(), config.model_path.c_str(),
      config.sensitivity.c_str(), config.audio_gain,
      config.apply_frontend ? 1 : 0, &handle, &info);
  local_result.error = MapCreateStatus(status);
  local_result.sample_rate_hz = info.sample_rate_hz;
  local_result.channels = info.channels;
  local_result.bits_per_sample = info.bits_per_sample;
  local_result.keyword_count = info.keyword_count;
  if (status != BOOMPI_SNOWBOY_LEGACY_OK || handle == nullptr) {
    if (status == BOOMPI_SNOWBOY_LEGACY_OK) {
      local_result.error =
          SnowboyWakeWordCreateError::kBackendException;
    }
    boompi_snowboy_legacy_destroy(handle);
    if (result != nullptr) {
      *result = local_result;
    }
    return nullptr;
  }

  if (info.sample_rate_hz != 16000U || info.channels != 1U ||
      info.bits_per_sample != 16U) {
    local_result.error =
        SnowboyWakeWordCreateError::kUnsupportedAudioFormat;
    boompi_snowboy_legacy_destroy(handle);
    if (result != nullptr) {
      *result = local_result;
    }
    return nullptr;
  }
  if (info.keyword_count == 0U) {
    local_result.error = SnowboyWakeWordCreateError::kInvalidKeywordCount;
    boompi_snowboy_legacy_destroy(handle);
    if (result != nullptr) {
      *result = local_result;
    }
    return nullptr;
  }

  auto engine = std::unique_ptr<SnowboyWakeWordEngine>(
      new (std::nothrow) SnowboyWakeWordEngine(
          handle, info.keyword_count));
  if (!engine) {
    local_result.error = SnowboyWakeWordCreateError::kAllocationFailed;
    boompi_snowboy_legacy_destroy(handle);
    if (result != nullptr) {
      *result = local_result;
    }
    return nullptr;
  }

  local_result.error = SnowboyWakeWordCreateError::kNone;
  if (result != nullptr) {
    *result = local_result;
  }
  return engine;
}

SnowboyWakeWordEngine::SnowboyWakeWordEngine(
    void* const backend_handle,
    const std::uint16_t keyword_count) noexcept
    : backend_handle_(backend_handle), keyword_count_(keyword_count) {}

SnowboyWakeWordEngine::~SnowboyWakeWordEngine() noexcept {
  boompi_snowboy_legacy_destroy(
      static_cast<BoompiSnowboyLegacyHandle*>(backend_handle_));
  backend_handle_ = nullptr;
}

audio::WakeWordResetResult SnowboyWakeWordEngine::Reset(
    const audio::WakeWordResetReason reason) noexcept {
  (void)reason;
  if (backend_handle_ == nullptr) {
    fault_error_ = WakeWordError::kNotInitialized;
    return {fault_error_, false};
  }

  int reset = 0;
  const BoompiSnowboyLegacyStatus status = boompi_snowboy_legacy_reset(
      static_cast<BoompiSnowboyLegacyHandle*>(backend_handle_), &reset);
  if (status == BOOMPI_SNOWBOY_LEGACY_OK && reset != 0) {
    fault_error_ = WakeWordError::kNone;
    return {WakeWordError::kNone, true};
  }

  fault_error_ =
      status == BOOMPI_SNOWBOY_LEGACY_BACKEND_EXCEPTION
          ? WakeWordError::kBackendException
          : WakeWordError::kResetFailed;
  return {fault_error_, false};
}

audio::WakeWordProcessResult SnowboyWakeWordEngine::Process(
    const Mono16kFrame& frame) noexcept {
  if (!HasExactMono16kContract(frame)) {
    return kInvalidFrame;
  }
  if (backend_handle_ == nullptr) {
    return LatchBackendFailure(WakeWordError::kNotInitialized);
  }
  if (fault_error_ != WakeWordError::kNone) {
    return {WakeWordDecision::kFaulted, fault_error_,
            0U, 0U, false};
  }

  std::int32_t detection_result = 0;
  const BoompiSnowboyLegacyStatus status =
      boompi_snowboy_legacy_process_s16(
          static_cast<BoompiSnowboyLegacyHandle*>(backend_handle_),
          frame.samples.data(), frame.samples_per_channel,
          &detection_result);
  if (status == BOOMPI_SNOWBOY_LEGACY_BACKEND_EXCEPTION) {
    return LatchBackendFailure(WakeWordError::kBackendException);
  }
  if (status != BOOMPI_SNOWBOY_LEGACY_OK) {
    return LatchBackendFailure(WakeWordError::kBackendRejectedAudio);
  }

  if (detection_result == -2) {
    return kSilence;
  }
  if (detection_result == 0) {
    return kNoEvent;
  }
  if (detection_result < 0) {
    return LatchBackendFailure(WakeWordError::kBackendRejectedAudio);
  }
  if (detection_result > static_cast<std::int32_t>(keyword_count_) ||
      detection_result >
          static_cast<std::int32_t>(
              std::numeric_limits<std::uint16_t>::max())) {
    return LatchBackendFailure(WakeWordError::kUnexpectedKeywordIndex);
  }

  return {WakeWordDecision::kDetected, WakeWordError::kNone,
          static_cast<std::uint16_t>(detection_result),
          0U, false};
}

std::uint16_t SnowboyWakeWordEngine::keyword_count() const noexcept {
  return keyword_count_;
}

audio::WakeWordProcessResult
SnowboyWakeWordEngine::LatchBackendFailure(
    const WakeWordError error) noexcept {
  fault_error_ = error;
  return {WakeWordDecision::kBackendError, error,
          0U, 0U, false};
}

}  // namespace boompi::platform::rv1106
