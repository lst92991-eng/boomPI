#include "boompi/platform/rv1106/alsa_pcm_playback.h"

#include <alsa/asoundlib.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <ctime>
#include <limits>
#include <string>

namespace boompi::platform::rv1106 {
namespace {

snd_pcm_t *Handle(void *const handle) noexcept {
  return static_cast<snd_pcm_t *>(handle);
}

snd_pcm_status_t *StatusStorage(void *const status) noexcept {
  return static_cast<snd_pcm_status_t *>(status);
}

PcmPlaybackDeviceCode MapAlsaError(const std::int32_t error) noexcept {
  switch (error) {
  case -EAGAIN:
    return PcmPlaybackDeviceCode::kWouldBlock;
  case -EINTR:
    return PcmPlaybackDeviceCode::kInterrupted;
  case -EPIPE:
    return PcmPlaybackDeviceCode::kXrun;
  case -ESTRPIPE:
    return PcmPlaybackDeviceCode::kSuspended;
  case -ENODEV:
  case -ENXIO:
  case -ENOTTY:
    return PcmPlaybackDeviceCode::kDeviceLost;
  case -ENOENT:
    return PcmPlaybackDeviceCode::kUnavailable;
  default:
    return PcmPlaybackDeviceCode::kFatal;
  }
}

std::int32_t BoundedNativeError(const snd_pcm_sframes_t error) noexcept {
  if (error < std::numeric_limits<std::int32_t>::min()) {
    return std::numeric_limits<std::int32_t>::min();
  }
  if (error > std::numeric_limits<std::int32_t>::max()) {
    return std::numeric_limits<std::int32_t>::max();
  }
  return static_cast<std::int32_t>(error);
}

PcmPlaybackDeviceStatusResult StatusError(const std::int32_t error) noexcept {
  return {MapAlsaError(error), 0U, 0U, error};
}

PcmPlaybackDeviceControlResult ControlResult(const int result) noexcept {
  if (result >= 0) {
    return {PcmPlaybackDeviceCode::kSucceeded, 0};
  }
  const auto native_error = static_cast<std::int32_t>(result);
  return {MapAlsaError(native_error), native_error};
}

Status AlsaStatus(const char *const operation, const int error) {
  return Status::Error(StatusCode::kInternal,
                       std::string(operation) +
                           " failed: " + snd_strerror(error));
}

bool ValidOpenConfig(const AlsaPcmPlaybackConfig &config) noexcept {
  return !config.device_name.empty() && config.device_name.size() <= 255U &&
         config.device.valid() &&
         config.device.sample_rate_hz ==
             audio::AcceptedRenderChunk48k::kSampleRateHz;
}

PcmPlaybackDeviceCode CodeForState(const snd_pcm_state_t state) noexcept {
  switch (state) {
  case SND_PCM_STATE_PREPARED:
  case SND_PCM_STATE_RUNNING:
    return PcmPlaybackDeviceCode::kSucceeded;
  case SND_PCM_STATE_XRUN:
    return PcmPlaybackDeviceCode::kXrun;
  case SND_PCM_STATE_SUSPENDED:
    return PcmPlaybackDeviceCode::kSuspended;
  case SND_PCM_STATE_DISCONNECTED:
    return PcmPlaybackDeviceCode::kDeviceLost;
  case SND_PCM_STATE_OPEN:
  case SND_PCM_STATE_SETUP:
  case SND_PCM_STATE_DRAINING:
  case SND_PCM_STATE_PAUSED:
  case SND_PCM_STATE_PRIVATE1:
    return PcmPlaybackDeviceCode::kFatal;
  }
  return PcmPlaybackDeviceCode::kFatal;
}

std::int32_t NativeErrorForState(const snd_pcm_state_t state) noexcept {
  switch (state) {
  case SND_PCM_STATE_XRUN:
    return -EPIPE;
  case SND_PCM_STATE_SUSPENDED:
    return -ESTRPIPE;
  case SND_PCM_STATE_DISCONNECTED:
    return -ENODEV;
  default:
    return -EBADFD;
  }
}

} // namespace

std::uint64_t AlsaPcmPlaybackDevice::MonotonicNowUs() const noexcept {
  timespec timestamp{};
  if (clock_gettime(CLOCK_MONOTONIC, &timestamp) != 0 || timestamp.tv_sec < 0 ||
      timestamp.tv_nsec < 0 || timestamp.tv_nsec >= 1000000000L) {
    return 0U;
  }
  const auto seconds = static_cast<std::uint64_t>(timestamp.tv_sec);
  constexpr std::uint64_t kMicrosecondsPerSecond = 1000000U;
  if (seconds >
      std::numeric_limits<std::uint64_t>::max() / kMicrosecondsPerSecond) {
    return 0U;
  }
  return seconds * kMicrosecondsPerSecond +
         static_cast<std::uint64_t>(timestamp.tv_nsec) / 1000U;
}

AlsaPcmPlaybackDevice::~AlsaPcmPlaybackDevice() noexcept { Close(); }

Status AlsaPcmPlaybackDevice::Open(const AlsaPcmPlaybackConfig &config,
                                   AlsaPcmPlaybackDevice *const output) {
  if (output == nullptr) {
    return Status::Error(StatusCode::kInvalidArgument,
                         "ALSA playback output must not be null");
  }
  if (output->is_open()) {
    return Status::Error(StatusCode::kFailedPrecondition,
                         "ALSA playback device is already open");
  }
  if (!ValidOpenConfig(config)) {
    return Status::Error(
        StatusCode::kInvalidArgument,
        "ALSA playback requires an explicit named PCM and valid exact 48 kHz "
        "channels/period/buffer/threshold contract");
  }

  snd_pcm_t *handle = nullptr;
  constexpr int kOpenMode = SND_PCM_NONBLOCK | SND_PCM_NO_AUTO_RESAMPLE |
                            SND_PCM_NO_AUTO_CHANNELS | SND_PCM_NO_AUTO_FORMAT;
  int error = snd_pcm_open(&handle, config.device_name.c_str(),
                           SND_PCM_STREAM_PLAYBACK, kOpenMode);
  if (error < 0) {
    return AlsaStatus("ALSA playback open", error);
  }

  const auto close_handle = [&handle]() noexcept {
    if (handle != nullptr) {
      static_cast<void>(snd_pcm_close(handle));
      handle = nullptr;
    }
  };

  error = snd_pcm_nonblock(handle, 1);
  if (error < 0) {
    close_handle();
    return AlsaStatus("ALSA playback nonblocking mode", error);
  }

  snd_pcm_hw_params_t *hardware_params = nullptr;
  snd_pcm_hw_params_alloca(&hardware_params);
  error = snd_pcm_hw_params_any(handle, hardware_params);
  if (error >= 0) {
    error = snd_pcm_hw_params_set_access(handle, hardware_params,
                                         SND_PCM_ACCESS_RW_INTERLEAVED);
  }
  if (error >= 0) {
    error = snd_pcm_hw_params_set_format(handle, hardware_params,
                                         SND_PCM_FORMAT_S16_LE);
  }
  if (error >= 0) {
    error = snd_pcm_hw_params_set_rate_resample(handle, hardware_params, 0U);
  }
  if (error >= 0) {
    error = snd_pcm_hw_params_set_channels(handle, hardware_params,
                                           config.device.channels);
  }
  if (error >= 0) {
    error = snd_pcm_hw_params_set_rate(handle, hardware_params,
                                       config.device.sample_rate_hz, 0);
  }

  snd_pcm_uframes_t period_sample_frames = config.device.period_sample_frames;
  int period_direction = 0;
  if (error >= 0) {
    error = snd_pcm_hw_params_set_period_size_near(
        handle, hardware_params, &period_sample_frames, &period_direction);
  }
  snd_pcm_uframes_t buffer_sample_frames = config.device.buffer_sample_frames;
  if (error >= 0) {
    error = snd_pcm_hw_params_set_buffer_size_near(handle, hardware_params,
                                                   &buffer_sample_frames);
  }
  if (error >= 0) {
    error = snd_pcm_hw_params(handle, hardware_params);
  }

  unsigned int actual_rate_hz = 0U;
  unsigned int actual_channels = 0U;
  if (error >= 0) {
    error = snd_pcm_hw_params_get_rate(hardware_params, &actual_rate_hz,
                                       &period_direction);
  }
  if (error >= 0) {
    error = snd_pcm_hw_params_get_channels(hardware_params, &actual_channels);
  }
  if (error >= 0) {
    error = snd_pcm_hw_params_get_period_size(
        hardware_params, &period_sample_frames, &period_direction);
  }
  if (error >= 0) {
    error = snd_pcm_hw_params_get_buffer_size(hardware_params,
                                              &buffer_sample_frames);
  }
  if (error < 0) {
    close_handle();
    return AlsaStatus("ALSA playback hardware configuration", error);
  }

  const bool exact_named_pcm_contract =
      actual_rate_hz == config.device.sample_rate_hz &&
      actual_channels == config.device.channels &&
      period_sample_frames == config.device.period_sample_frames &&
      buffer_sample_frames == config.device.buffer_sample_frames;
  if (!exact_named_pcm_contract) {
    close_handle();
    return Status::Error(
        StatusCode::kNotSupported,
        "ALSA playback named PCM did not accept the exact configured hw "
        "parameters");
  }

  snd_pcm_sw_params_t *software_params = nullptr;
  snd_pcm_sw_params_alloca(&software_params);
  error = snd_pcm_sw_params_current(handle, software_params);
  if (error >= 0) {
    error = snd_pcm_sw_params_set_avail_min(
        handle, software_params, config.device.avail_min_sample_frames);
  }
  if (error >= 0) {
    error = snd_pcm_sw_params_set_start_threshold(
        handle, software_params, config.device.start_threshold_sample_frames);
  }
  if (error >= 0) {
    error = snd_pcm_sw_params_set_tstamp_mode(handle, software_params,
                                              SND_PCM_TSTAMP_ENABLE);
  }
  if (error >= 0) {
    error = snd_pcm_sw_params_set_tstamp_type(handle, software_params,
                                              SND_PCM_TSTAMP_TYPE_MONOTONIC);
  }
  if (error >= 0) {
    error = snd_pcm_sw_params(handle, software_params);
  }

  snd_pcm_uframes_t actual_avail_min = 0U;
  snd_pcm_uframes_t actual_start_threshold = 0U;
  snd_pcm_tstamp_t actual_tstamp_mode = SND_PCM_TSTAMP_NONE;
  snd_pcm_tstamp_type_t actual_tstamp_type = SND_PCM_TSTAMP_TYPE_GETTIMEOFDAY;
  if (error >= 0) {
    error = snd_pcm_sw_params_get_avail_min(software_params, &actual_avail_min);
  }
  if (error >= 0) {
    error = snd_pcm_sw_params_get_start_threshold(software_params,
                                                  &actual_start_threshold);
  }
  if (error >= 0) {
    error =
        snd_pcm_sw_params_get_tstamp_mode(software_params, &actual_tstamp_mode);
  }
  if (error >= 0) {
    error =
        snd_pcm_sw_params_get_tstamp_type(software_params, &actual_tstamp_type);
  }
  if (error < 0) {
    close_handle();
    return AlsaStatus("ALSA playback software configuration", error);
  }
  if (actual_avail_min != config.device.avail_min_sample_frames ||
      actual_start_threshold != config.device.start_threshold_sample_frames ||
      actual_tstamp_mode != SND_PCM_TSTAMP_ENABLE ||
      actual_tstamp_type != SND_PCM_TSTAMP_TYPE_MONOTONIC) {
    close_handle();
    return Status::Error(
        StatusCode::kNotSupported,
        "ALSA playback did not accept the exact configured software "
        "threshold contract");
  }

  snd_pcm_status_t *status = nullptr;
  error = snd_pcm_status_malloc(&status);
  if (error < 0) {
    close_handle();
    return AlsaStatus("ALSA playback status allocation", error);
  }

  output->handle_ = handle;
  output->status_ = status;
  output->actual_config_ = config.device;
  handle = nullptr;
  return Status::Ok();
}

PcmPlaybackDeviceStatusResult AlsaPcmPlaybackDevice::QueryStatus() noexcept {
  if (!is_open() || status_ == nullptr) {
    return {PcmPlaybackDeviceCode::kUnavailable, 0U, 0U, 0};
  }

  const int status_result =
      snd_pcm_status(Handle(handle_), StatusStorage(status_));
  if (status_result < 0) {
    return StatusError(static_cast<std::int32_t>(status_result));
  }

  const snd_pcm_state_t state =
      snd_pcm_status_get_state(StatusStorage(status_));
  const PcmPlaybackDeviceCode state_code = CodeForState(state);
  if (state_code != PcmPlaybackDeviceCode::kSucceeded) {
    return {state_code, 0U, 0U, NativeErrorForState(state)};
  }

  const snd_pcm_sframes_t delay =
      snd_pcm_status_get_delay(StatusStorage(status_));
  if (delay < 0 ||
      static_cast<std::uint64_t>(delay) >
          audio::PcmPlaybackWriteResult::kMaximumQueuedSampleFrames) {
    return {PcmPlaybackDeviceCode::kFatal, 0U, 0U, 0};
  }

  snd_htimestamp_t timestamp{};
  snd_pcm_status_get_htstamp(StatusStorage(status_), &timestamp);
  if (timestamp.tv_sec < 0 || timestamp.tv_nsec < 0 ||
      timestamp.tv_nsec >= 1000000000L) {
    return {PcmPlaybackDeviceCode::kFatal, 0U, 0U, 0};
  }
  const auto seconds = static_cast<std::uint64_t>(timestamp.tv_sec);
  constexpr std::uint64_t kMicrosecondsPerSecond = 1000000U;
  if (seconds >
      std::numeric_limits<std::uint64_t>::max() / kMicrosecondsPerSecond) {
    return {PcmPlaybackDeviceCode::kFatal, 0U, 0U, 0};
  }
  const std::uint64_t timestamp_us =
      seconds * kMicrosecondsPerSecond +
      static_cast<std::uint64_t>(timestamp.tv_nsec) / 1000U;
  if (timestamp_us == 0U) {
    return {PcmPlaybackDeviceCode::kFatal, 0U, 0U, 0};
  }

  return {PcmPlaybackDeviceCode::kSucceeded, static_cast<std::uint32_t>(delay),
          timestamp_us, 0,
          state == SND_PCM_STATE_PREPARED ? PcmPlaybackTimelineState::kPrepared
                                          : PcmPlaybackTimelineState::kRunning};
}

PcmPlaybackDeviceWriteResult AlsaPcmPlaybackDevice::TryWriteInterleavedS16(
    const std::int16_t *const samples,
    const std::uint32_t sample_frames) noexcept {
  if (!is_open()) {
    return {PcmPlaybackDeviceCode::kUnavailable, 0U, 0};
  }
  if (samples == nullptr || sample_frames == 0U) {
    return {PcmPlaybackDeviceCode::kFatal, 0U, 0};
  }

  const snd_pcm_sframes_t result =
      snd_pcm_writei(Handle(handle_), samples, sample_frames);
  if (result > 0) {
    const auto accepted = static_cast<std::uint64_t>(result);
    return {PcmPlaybackDeviceCode::kSucceeded,
            static_cast<std::uint32_t>(std::min<std::uint64_t>(
                accepted, std::numeric_limits<std::uint32_t>::max())),
            0};
  }
  if (result == 0) {
    return {PcmPlaybackDeviceCode::kZeroProgress, 0U, 0};
  }

  const std::int32_t native_error = BoundedNativeError(result);
  return {MapAlsaError(native_error), 0U, native_error};
}

PcmPlaybackDeviceControlResult AlsaPcmPlaybackDevice::Drop() noexcept {
  if (!is_open()) {
    return {PcmPlaybackDeviceCode::kUnavailable, 0};
  }
  return ControlResult(snd_pcm_drop(Handle(handle_)));
}

PcmPlaybackDeviceControlResult AlsaPcmPlaybackDevice::Prepare() noexcept {
  if (!is_open()) {
    return {PcmPlaybackDeviceCode::kUnavailable, 0};
  }
  return ControlResult(snd_pcm_prepare(Handle(handle_)));
}

void AlsaPcmPlaybackDevice::Close() noexcept {
  if (status_ != nullptr) {
    snd_pcm_status_free(StatusStorage(status_));
    status_ = nullptr;
  }
  if (is_open()) {
    static_cast<void>(snd_pcm_close(Handle(handle_)));
    handle_ = nullptr;
  }
  actual_config_ = PcmPlaybackDeviceConfig{};
}

} // namespace boompi::platform::rv1106
