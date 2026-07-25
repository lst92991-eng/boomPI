#include "boompi/config/client_config.h"

namespace boompi::config {

ClientConfig DefaultClientConfig() { return ClientConfig{}; }

Status ValidateClientConfig(const ClientConfig& config) {
  if (config.audio.capture_sample_rate_hz != 48000U ||
      config.audio.dsp_sample_rate_hz != 16000U ||
      config.audio.playback_sample_rate_hz != 48000U) {
    return Status::Error(StatusCode::kInvalidArgument,
                         "protocol v1 requires 48/16/48 kHz audio rates");
  }
  if (config.audio.frame_duration_ms != 20U) {
    return Status::Error(StatusCode::kInvalidArgument,
                         "protocol v1 requires 20 ms audio frames");
  }
  if (config.audio.capture_channels != 2U &&
      config.audio.capture_channels != 4U) {
    return Status::Error(StatusCode::kInvalidArgument,
                         "protocol v1 capture must use 2 or 4 channels");
  }
  if (config.audio.default_volume_percent > 100U) {
    return Status::Error(StatusCode::kInvalidArgument,
                         "default volume must be between 0 and 100");
  }
  if (config.event_queue_capacity == 0U ||
      config.event_queue_capacity > 4096U) {
    return Status::Error(StatusCode::kInvalidArgument,
                         "event queue capacity must be between 1 and 4096");
  }
  if (config.follow_up_timeout_ms == 0U ||
      config.initial_speech_timeout_ms == 0U ||
      config.end_of_speech_ms == 0U) {
    return Status::Error(StatusCode::kInvalidArgument,
                         "conversation timeouts must be non-zero");
  }
  return Status::Ok();
}

}  // namespace boompi::config
