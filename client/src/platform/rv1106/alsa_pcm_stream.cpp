#include "boompi/platform/rv1106/alsa_pcm_stream.h"

#include <alsa/asoundlib.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <limits>
#include <string>

namespace boompi::platform::rv1106 {
namespace {

snd_pcm_t* Handle(void* const handle) noexcept {
  return static_cast<snd_pcm_t*>(handle);
}

PcmBackendResult MapTransferResult(
    const snd_pcm_sframes_t result) noexcept {
  if (result > 0) {
    const auto frames = static_cast<std::uint64_t>(result);
    if (frames > std::numeric_limits<std::uint32_t>::max()) {
      return {PcmBackendCode::kFatal, 0U};
    }
    return {PcmBackendCode::kFrames,
            static_cast<std::uint32_t>(frames)};
  }
  if (result == -EAGAIN || result == 0) {
    return {PcmBackendCode::kWouldBlock, 0U};
  }
  if (result == -EINTR) {
    return {PcmBackendCode::kInterrupted, 0U};
  }
  if (result == -EPIPE) {
    return {PcmBackendCode::kXrun, 0U};
  }
  if (result == -ESTRPIPE) {
    return {PcmBackendCode::kSuspended, 0U};
  }
  return {PcmBackendCode::kFatal, 0U};
}

Status AlsaStatus(const std::string& operation, const int error) {
  return Status::Error(StatusCode::kInternal,
                       operation + " failed: " + snd_strerror(error));
}

bool ValidConfig(const AlsaPcmStreamConfig& config) noexcept {
  const bool channels_valid =
      config.direction == AlsaPcmDirection::kPlayback
          ? config.channels == 2U
          : (config.channels == 2U || config.channels == 4U);
  return !config.device_name.empty() && config.sample_rate_hz == 48000U &&
         channels_valid && config.period_sample_frames == 960U &&
         config.buffer_sample_frames >=
             config.period_sample_frames * 2U &&
         config.buffer_sample_frames <= 4096U;
}

}  // namespace

AlsaPcmStream::~AlsaPcmStream() {
  Close();
}

Status AlsaPcmStream::Open(const AlsaPcmStreamConfig& config,
                           AlsaPcmStream* const output) {
  if (output == nullptr) {
    return Status::Error(StatusCode::kInvalidArgument,
                         "ALSA PCM output must not be null");
  }
  if (output->is_open()) {
    return Status::Error(StatusCode::kFailedPrecondition,
                         "ALSA PCM stream is already open");
  }
  if (!ValidConfig(config)) {
    return Status::Error(
        StatusCode::kInvalidArgument,
        "ALSA PCM requires explicit device, 48 kHz, 960-frame period, "
        "bounded buffer, and verified channel count");
  }

  snd_pcm_t* handle = nullptr;
  const snd_pcm_stream_t stream =
      config.direction == AlsaPcmDirection::kCapture
          ? SND_PCM_STREAM_CAPTURE
          : SND_PCM_STREAM_PLAYBACK;
  int error = snd_pcm_open(&handle, config.device_name.c_str(), stream,
                           SND_PCM_NONBLOCK);
  if (error < 0) {
    return AlsaStatus("ALSA PCM open", error);
  }

  snd_pcm_hw_params_t* hardware_params = nullptr;
  snd_pcm_hw_params_alloca(&hardware_params);
  error = snd_pcm_hw_params_any(handle, hardware_params);
  if (error >= 0) {
    error = snd_pcm_hw_params_set_access(
        handle, hardware_params, SND_PCM_ACCESS_RW_INTERLEAVED);
  }
  if (error >= 0) {
    error = snd_pcm_hw_params_set_format(handle, hardware_params,
                                         SND_PCM_FORMAT_S16_LE);
  }
  if (error >= 0) {
    error = snd_pcm_hw_params_set_channels(handle, hardware_params,
                                           config.channels);
  }

  unsigned int rate_hz = config.sample_rate_hz;
  int rate_direction = 0;
  if (error >= 0) {
    error = snd_pcm_hw_params_set_rate_near(
        handle, hardware_params, &rate_hz, &rate_direction);
  }

  snd_pcm_uframes_t period_sample_frames = config.period_sample_frames;
  int period_direction = 0;
  if (error >= 0) {
    error = snd_pcm_hw_params_set_period_size_near(
        handle, hardware_params, &period_sample_frames, &period_direction);
  }

  snd_pcm_uframes_t buffer_sample_frames = config.buffer_sample_frames;
  if (error >= 0) {
    error = snd_pcm_hw_params_set_buffer_size_near(
        handle, hardware_params, &buffer_sample_frames);
  }
  if (error >= 0) {
    error = snd_pcm_hw_params(handle, hardware_params);
  }
  if (error >= 0) {
    error = snd_pcm_hw_params_get_rate(hardware_params, &rate_hz,
                                       &rate_direction);
  }
  unsigned int channels = 0U;
  if (error >= 0) {
    error = snd_pcm_hw_params_get_channels(hardware_params, &channels);
  }
  if (error >= 0) {
    error = snd_pcm_hw_params_get_period_size(
        hardware_params, &period_sample_frames, &period_direction);
  }
  if (error >= 0) {
    error = snd_pcm_hw_params_get_buffer_size(hardware_params,
                                              &buffer_sample_frames);
  }

  const bool exact_contract =
      error >= 0 && rate_hz == config.sample_rate_hz &&
      channels == config.channels &&
      period_sample_frames == config.period_sample_frames &&
      buffer_sample_frames == config.buffer_sample_frames;
  if (!exact_contract) {
    snd_pcm_close(handle);
    if (error < 0) {
      return AlsaStatus("ALSA PCM hardware configuration", error);
    }
    return Status::Error(
        StatusCode::kNotSupported,
        "ALSA PCM did not accept the exact configured rate/channels/period/"
        "buffer contract");
  }

  snd_pcm_sw_params_t* software_params = nullptr;
  snd_pcm_sw_params_alloca(&software_params);
  error = snd_pcm_sw_params_current(handle, software_params);
  if (error >= 0) {
    error = snd_pcm_sw_params_set_avail_min(
        handle, software_params, period_sample_frames);
  }
  if (error >= 0) {
    const snd_pcm_uframes_t start_threshold =
        config.direction == AlsaPcmDirection::kPlayback
            ? period_sample_frames
            : 1U;
    error = snd_pcm_sw_params_set_start_threshold(
        handle, software_params, start_threshold);
  }
  if (error >= 0) {
    error = snd_pcm_sw_params(handle, software_params);
  }
  if (error >= 0) {
    error = snd_pcm_prepare(handle);
  }
  if (error < 0) {
    snd_pcm_close(handle);
    return AlsaStatus("ALSA PCM software configuration", error);
  }

  output->handle_ = handle;
  output->direction_ = config.direction;
  output->actual_config_.sample_rate_hz = rate_hz;
  output->actual_config_.channels = static_cast<std::uint8_t>(channels);
  output->actual_config_.period_sample_frames =
      static_cast<std::uint32_t>(period_sample_frames);
  output->actual_config_.buffer_sample_frames =
      static_cast<std::uint32_t>(buffer_sample_frames);
  return Status::Ok();
}

const AlsaPcmActualConfig& AlsaPcmStream::actual_config() const noexcept {
  return actual_config_;
}

bool AlsaPcmStream::is_open() const noexcept {
  return handle_ != nullptr;
}

PcmBackendResult AlsaPcmStream::Wait(
    const std::uint32_t timeout_ms) noexcept {
  if (!is_open() || timeout_ms == 0U ||
      timeout_ms > static_cast<std::uint32_t>(
                       std::numeric_limits<int>::max())) {
    return {PcmBackendCode::kFatal, 0U};
  }
  const int result = snd_pcm_wait(Handle(handle_),
                                  static_cast<int>(timeout_ms));
  if (result > 0) {
    return {PcmBackendCode::kReady, 0U};
  }
  if (result == 0) {
    return {PcmBackendCode::kTimeout, 0U};
  }
  return MapTransferResult(result);
}

PcmBackendResult AlsaPcmStream::ReadInterleaved(
    std::int16_t* const samples,
    const std::uint32_t requested_sample_frames) noexcept {
  if (!is_open() || direction_ != AlsaPcmDirection::kCapture ||
      samples == nullptr || requested_sample_frames == 0U) {
    return {PcmBackendCode::kFatal, 0U};
  }
  if (snd_pcm_state(Handle(handle_)) == SND_PCM_STATE_PREPARED) {
    const int start_result = snd_pcm_start(Handle(handle_));
    if (start_result < 0) {
      return MapTransferResult(start_result);
    }
    // Do not read immediately after start: the nonblocking driver can expose
    // a sub-period prefix before avail_min is reached. Let the bounded Wait
    // path establish one complete 20 ms capture period first.
    return {PcmBackendCode::kWouldBlock, 0U};
  }
  const snd_pcm_sframes_t available = snd_pcm_avail_update(Handle(handle_));
  if (available < 0) {
    return MapTransferResult(available);
  }
  if (static_cast<std::uint64_t>(available) < requested_sample_frames) {
    return {PcmBackendCode::kWouldBlock, 0U};
  }
  return MapTransferResult(snd_pcm_readi(Handle(handle_), samples,
                                         requested_sample_frames));
}

PcmBackendResult AlsaPcmStream::WriteInterleaved(
    const std::int16_t* const samples,
    const std::uint32_t requested_sample_frames) noexcept {
  if (!is_open() || direction_ != AlsaPcmDirection::kPlayback ||
      samples == nullptr || requested_sample_frames == 0U) {
    return {PcmBackendCode::kFatal, 0U};
  }
  return MapTransferResult(snd_pcm_writei(Handle(handle_), samples,
                                          requested_sample_frames));
}

bool AlsaPcmStream::Recover(const PcmBackendCode reason) noexcept {
  if (!is_open() ||
      (reason != PcmBackendCode::kXrun &&
       reason != PcmBackendCode::kSuspended)) {
    return false;
  }
  if (reason == PcmBackendCode::kXrun) {
    return snd_pcm_prepare(Handle(handle_)) >= 0;
  }

  // snd_pcm_recover may sleep/retry resume indefinitely. One resume attempt
  // followed by prepare keeps this worker-owned error path bounded.
  const int resume_result = snd_pcm_resume(Handle(handle_));
  return resume_result >= 0 || snd_pcm_prepare(Handle(handle_)) >= 0;
}

bool AlsaPcmStream::Drop() noexcept {
  return is_open() && snd_pcm_drop(Handle(handle_)) >= 0;
}

bool AlsaPcmStream::Prepare() noexcept {
  return is_open() && snd_pcm_prepare(Handle(handle_)) >= 0;
}

bool AlsaPcmStream::Reset() noexcept {
  const bool dropped = Drop();
  const bool prepared = Prepare();
  return dropped && prepared;
}

bool AlsaPcmStream::QueryQueuedSampleFrames(
    std::uint32_t* const queued_sample_frames) noexcept {
  if (queued_sample_frames == nullptr || !is_open() ||
      direction_ != AlsaPcmDirection::kPlayback) {
    return false;
  }
  snd_pcm_sframes_t delay_sample_frames = 0;
  if (snd_pcm_delay(Handle(handle_), &delay_sample_frames) < 0) {
    return false;
  }
  if (delay_sample_frames <= 0) {
    *queued_sample_frames = 0U;
    return true;
  }
  const std::uint64_t delay =
      static_cast<std::uint64_t>(delay_sample_frames);
  *queued_sample_frames = static_cast<std::uint32_t>(
      std::min<std::uint64_t>(delay, std::numeric_limits<std::uint32_t>::max()));
  return true;
}

void AlsaPcmStream::Close() noexcept {
  if (is_open()) {
    snd_pcm_close(Handle(handle_));
  }
  handle_ = nullptr;
  actual_config_ = AlsaPcmActualConfig{};
}

}  // namespace boompi::platform::rv1106
