#include "boompi/platform/rv1106/alsa_single_turn_io.h"

#include <alsa/asoundlib.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <ctime>
#include <limits>
#include <string>

namespace boompi::platform::rv1106 {
namespace {

snd_pcm_t* Handle(void* const handle) noexcept {
  return static_cast<snd_pcm_t*>(handle);
}

Status AlsaError(const char* const operation, const int error) {
  return Status::Error(StatusCode::kInternal,
                       std::string(operation) + " failed: " +
                           snd_strerror(error));
}

bool IsCanonicalUnsigned(const std::string& value,
                         const std::size_t begin,
                         const std::size_t end) noexcept {
  if (begin >= end || end > value.size() ||
      (end - begin > 1U && value[begin] == '0')) {
    return false;
  }
  for (std::size_t index = begin; index < end; ++index) {
    if (value[index] < '0' || value[index] > '9') {
      return false;
    }
  }
  return true;
}

bool IsExplicitPcmName(const std::string& value) noexcept {
#if defined(BOOMPI_ALLOW_ALSA_NULL_TEST_PCM)
  if (value == "null") {
    return true;
  }
#endif
  if (value.size() < 6U || value.size() > 31U ||
      value.compare(0U, 3U, "hw:") != 0) {
    return false;
  }
  const std::size_t comma = value.find(',', 3U);
  return comma != std::string::npos &&
         value.find(',', comma + 1U) == std::string::npos &&
         IsCanonicalUnsigned(value, 3U, comma) &&
         IsCanonicalUnsigned(value, comma + 1U, value.size());
}

std::uint64_t MonotonicMs() noexcept {
  timespec timestamp{};
  if (clock_gettime(CLOCK_MONOTONIC, &timestamp) != 0 ||
      timestamp.tv_sec < 0 || timestamp.tv_nsec < 0 ||
      timestamp.tv_nsec >= 1000000000L) {
    return 0U;
  }
  const auto seconds = static_cast<std::uint64_t>(timestamp.tv_sec);
  if (seconds > std::numeric_limits<std::uint64_t>::max() / 1000U) {
    return 0U;
  }
  return seconds * 1000U +
         static_cast<std::uint64_t>(timestamp.tv_nsec) / 1000000U;
}

bool MakeDeadline(const std::uint32_t timeout_ms,
                  std::uint64_t* const deadline_ms) noexcept {
  if (deadline_ms == nullptr || timeout_ms == 0U) {
    return false;
  }
  const std::uint64_t now_ms = MonotonicMs();
  if (now_ms == 0U ||
      now_ms > std::numeric_limits<std::uint64_t>::max() - timeout_ms) {
    return false;
  }
  *deadline_ms = now_ms + timeout_ms;
  return true;
}

int RemainingWaitMs(const std::uint64_t deadline_ms) noexcept {
  const std::uint64_t now_ms = MonotonicMs();
  if (now_ms == 0U || now_ms >= deadline_ms) {
    return 0;
  }
  const std::uint64_t remaining = deadline_ms - now_ms;
  return static_cast<int>(std::min<std::uint64_t>(
      remaining, static_cast<std::uint64_t>(std::numeric_limits<int>::max())));
}

Status WaitForPcm(snd_pcm_t* const handle,
                  const std::uint64_t deadline_ms,
                  const char* const operation) {
  for (;;) {
    const int remaining_ms = RemainingWaitMs(deadline_ms);
    if (remaining_ms <= 0) {
      return Status::Error(StatusCode::kResourceExhausted,
                           std::string(operation) + " timed out");
    }
    const int result = snd_pcm_wait(handle, remaining_ms);
    if (result > 0) {
      return Status::Ok();
    }
    if (result == 0) {
      return Status::Error(StatusCode::kResourceExhausted,
                           std::string(operation) + " timed out");
    }
    if (result != -EINTR) {
      return AlsaError(operation, result);
    }
  }
}

Status OpenExactPcm(const std::string& device_name,
                    const snd_pcm_stream_t stream,
                    snd_pcm_t** const output) {
  if (output == nullptr || *output != nullptr) {
    return Status::Error(StatusCode::kInvalidArgument,
                         "ALSA output handle is invalid");
  }
  constexpr int kOpenMode = SND_PCM_NONBLOCK | SND_PCM_NO_AUTO_RESAMPLE |
                            SND_PCM_NO_AUTO_CHANNELS |
                            SND_PCM_NO_AUTO_FORMAT;
  snd_pcm_t* handle = nullptr;
  int error = snd_pcm_open(&handle, device_name.c_str(), stream, kOpenMode);
  if (error < 0) {
    return AlsaError(stream == SND_PCM_STREAM_CAPTURE ? "ALSA capture open"
                                                      : "ALSA playback open",
                     error);
  }
  const auto close_handle = [&handle]() noexcept {
    if (handle != nullptr) {
      static_cast<void>(snd_pcm_close(handle));
      handle = nullptr;
    }
  };

  error = snd_pcm_nonblock(handle, 1);
  snd_pcm_hw_params_t* hardware = nullptr;
  snd_pcm_hw_params_alloca(&hardware);
  if (error >= 0) {
    error = snd_pcm_hw_params_any(handle, hardware);
  }
  if (error >= 0) {
    error = snd_pcm_hw_params_set_access(handle, hardware,
                                         SND_PCM_ACCESS_RW_INTERLEAVED);
  }
  if (error >= 0) {
    error = snd_pcm_hw_params_set_format(handle, hardware,
                                         SND_PCM_FORMAT_S16_LE);
  }
  if (error >= 0) {
    error = snd_pcm_hw_params_set_rate_resample(handle, hardware, 0U);
  }
  if (error >= 0) {
    error = snd_pcm_hw_params_set_channels(handle, hardware,
                                           AlsaSingleTurnIo::kChannels);
  }
  if (error >= 0) {
    error = snd_pcm_hw_params_set_rate(handle, hardware,
                                       AlsaSingleTurnIo::kSampleRateHz, 0);
  }
  snd_pcm_uframes_t period_frames = AlsaSingleTurnIo::kPeriodFrames;
  int direction = 0;
  if (error >= 0) {
    error = snd_pcm_hw_params_set_period_size_near(
        handle, hardware, &period_frames, &direction);
  }
  snd_pcm_uframes_t buffer_frames = AlsaSingleTurnIo::kBufferFrames;
  if (error >= 0) {
    error = snd_pcm_hw_params_set_buffer_size_near(handle, hardware,
                                                   &buffer_frames);
  }
  if (error >= 0) {
    error = snd_pcm_hw_params(handle, hardware);
  }

  unsigned int actual_rate = 0U;
  unsigned int actual_channels = 0U;
  if (error >= 0) {
    error = snd_pcm_hw_params_get_rate(hardware, &actual_rate, &direction);
  }
  if (error >= 0) {
    error = snd_pcm_hw_params_get_channels(hardware, &actual_channels);
  }
  if (error >= 0) {
    error = snd_pcm_hw_params_get_period_size(hardware, &period_frames,
                                              &direction);
  }
  if (error >= 0) {
    error = snd_pcm_hw_params_get_buffer_size(hardware, &buffer_frames);
  }
  if (error < 0) {
    close_handle();
    return AlsaError("ALSA hardware configuration", error);
  }
  if (actual_rate != AlsaSingleTurnIo::kSampleRateHz ||
      actual_channels != AlsaSingleTurnIo::kChannels ||
      period_frames != AlsaSingleTurnIo::kPeriodFrames ||
      buffer_frames != AlsaSingleTurnIo::kBufferFrames) {
    close_handle();
    return Status::Error(
        StatusCode::kNotSupported,
        "ALSA PCM did not accept the exact 48 kHz stereo 960/3840 contract");
  }

  snd_pcm_sw_params_t* software = nullptr;
  snd_pcm_sw_params_alloca(&software);
  error = snd_pcm_sw_params_current(handle, software);
  if (error >= 0) {
    error = snd_pcm_sw_params_set_avail_min(
        handle, software, AlsaSingleTurnIo::kPeriodFrames);
  }
  const snd_pcm_uframes_t start_threshold =
      stream == SND_PCM_STREAM_CAPTURE ? 1U
                                       : AlsaSingleTurnIo::kPeriodFrames;
  if (error >= 0) {
    error = snd_pcm_sw_params_set_start_threshold(handle, software,
                                                  start_threshold);
  }
  if (error >= 0) {
    error = snd_pcm_sw_params(handle, software);
  }
  snd_pcm_uframes_t actual_avail_min = 0U;
  snd_pcm_uframes_t actual_start_threshold = 0U;
  if (error >= 0) {
    error = snd_pcm_sw_params_get_avail_min(software, &actual_avail_min);
  }
  if (error >= 0) {
    error = snd_pcm_sw_params_get_start_threshold(
        software, &actual_start_threshold);
  }
  if (error < 0) {
    close_handle();
    return AlsaError("ALSA software configuration", error);
  }
  if (actual_avail_min != AlsaSingleTurnIo::kPeriodFrames ||
      actual_start_threshold != start_threshold) {
    close_handle();
    return Status::Error(
        StatusCode::kNotSupported,
        "ALSA PCM did not accept the exact software threshold contract");
  }

  error = snd_pcm_prepare(handle);
  if (error < 0) {
    close_handle();
    return AlsaError("ALSA prepare", error);
  }
  *output = handle;
  handle = nullptr;
  return Status::Ok();
}

}  // namespace

AlsaSingleTurnIo::~AlsaSingleTurnIo() noexcept { Abort(); }

Status AlsaSingleTurnIo::Open(const AlsaSingleTurnConfig& config,
                              AlsaSingleTurnIo* const output) {
  if (output == nullptr) {
    return Status::Error(StatusCode::kInvalidArgument,
                         "single-turn ALSA output must not be null");
  }
  if (output->is_open()) {
    return Status::Error(StatusCode::kFailedPrecondition,
                         "single-turn ALSA I/O is already open");
  }
  if (!IsExplicitPcmName(config.capture_device_name) ||
      !IsExplicitPcmName(config.playback_device_name)) {
    return Status::Error(
        StatusCode::kInvalidArgument,
        "single-turn ALSA requires explicit canonical hw:C,D PCM names");
  }

  snd_pcm_t* capture = nullptr;
  Status status = OpenExactPcm(config.capture_device_name,
                               SND_PCM_STREAM_CAPTURE, &capture);
  if (!status.ok()) {
    return status;
  }
  snd_pcm_t* playback = nullptr;
  status = OpenExactPcm(config.playback_device_name,
                        SND_PCM_STREAM_PLAYBACK, &playback);
  if (!status.ok()) {
    static_cast<void>(snd_pcm_close(capture));
    return status;
  }

  output->capture_handle_ = capture;
  output->playback_handle_ = playback;
  output->capture_started_ = false;
  output->capture_finished_ = false;
  return Status::Ok();
}

Status AlsaSingleTurnIo::Capture20Ms(CapturePeriod* const output,
                                     const std::uint32_t timeout_ms) {
  if (!is_open() || output == nullptr || capture_finished_) {
    return Status::Error(StatusCode::kFailedPrecondition,
                         "single-turn capture is unavailable");
  }
  std::uint64_t deadline_ms = 0U;
  if (!MakeDeadline(timeout_ms, &deadline_ms)) {
    return Status::Error(StatusCode::kInvalidArgument,
                         "single-turn capture timeout is invalid");
  }

  snd_pcm_t* const capture = Handle(capture_handle_);
  if (!capture_started_) {
    const int start_result = snd_pcm_start(capture);
    if (start_result < 0) {
      return AlsaError("ALSA capture start", start_result);
    }
    capture_started_ = true;
  }

  std::uint32_t offset_frames = 0U;
  while (offset_frames < kPeriodFrames) {
    const snd_pcm_sframes_t result = snd_pcm_readi(
        capture, output->data() +
                     static_cast<std::size_t>(offset_frames) * kChannels,
        kPeriodFrames - offset_frames);
    if (result > 0) {
      const auto accepted = static_cast<std::uint64_t>(result);
      if (accepted > kPeriodFrames - offset_frames) {
        return Status::Error(StatusCode::kInternal,
                             "ALSA capture returned too many frames");
      }
      offset_frames += static_cast<std::uint32_t>(accepted);
      continue;
    }
    if (result == 0) {
      return Status::Error(StatusCode::kInternal,
                           "ALSA capture made zero progress");
    }
    if (result != -EAGAIN && result != -EINTR) {
      return AlsaError("ALSA capture read", static_cast<int>(result));
    }
    const Status wait_status =
        WaitForPcm(capture, deadline_ms, "ALSA capture wait");
    if (!wait_status.ok()) {
      return wait_status;
    }
  }
  return Status::Ok();
}

Status AlsaSingleTurnIo::FinishCapture() {
  if (!is_open() || !capture_started_ || capture_finished_) {
    return Status::Error(StatusCode::kFailedPrecondition,
                         "single-turn capture cannot be finished");
  }
  const int result = snd_pcm_drop(Handle(capture_handle_));
  if (result < 0) {
    return AlsaError("ALSA capture stop", result);
  }
  capture_started_ = false;
  capture_finished_ = true;
  return Status::Ok();
}

Status AlsaSingleTurnIo::WriteMono48k(const std::int16_t* const samples,
                                      const std::uint16_t sample_frames,
                                      const std::uint32_t timeout_ms) {
  if (!is_open() || samples == nullptr || sample_frames == 0U ||
      sample_frames > kPeriodFrames) {
    return Status::Error(StatusCode::kInvalidArgument,
                         "single-turn playback frame is invalid");
  }
  std::uint64_t deadline_ms = 0U;
  if (!MakeDeadline(timeout_ms, &deadline_ms)) {
    return Status::Error(StatusCode::kInvalidArgument,
                         "single-turn playback timeout is invalid");
  }
  for (std::uint16_t frame = 0U; frame < sample_frames; ++frame) {
    const std::size_t offset = static_cast<std::size_t>(frame) * kChannels;
    playback_interleaved_scratch_[offset] = samples[frame];
    playback_interleaved_scratch_[offset + 1U] = samples[frame];
  }

  snd_pcm_t* const playback = Handle(playback_handle_);
  std::uint32_t offset_frames = 0U;
  while (offset_frames < sample_frames) {
    const snd_pcm_sframes_t result = snd_pcm_writei(
        playback,
        playback_interleaved_scratch_.data() +
            static_cast<std::size_t>(offset_frames) * kChannels,
        sample_frames - offset_frames);
    if (result > 0) {
      const auto accepted = static_cast<std::uint64_t>(result);
      if (accepted > sample_frames - offset_frames) {
        return Status::Error(StatusCode::kInternal,
                             "ALSA playback accepted too many frames");
      }
      offset_frames += static_cast<std::uint32_t>(accepted);
      continue;
    }
    if (result == 0) {
      return Status::Error(StatusCode::kInternal,
                           "ALSA playback made zero progress");
    }
    if (result != -EAGAIN && result != -EINTR) {
      return AlsaError("ALSA playback write", static_cast<int>(result));
    }
    const Status wait_status =
        WaitForPcm(playback, deadline_ms, "ALSA playback wait");
    if (!wait_status.ok()) {
      return wait_status;
    }
  }
  return Status::Ok();
}

Status AlsaSingleTurnIo::DrainPlayback(const std::uint32_t timeout_ms) {
  if (!is_open()) {
    return Status::Error(StatusCode::kFailedPrecondition,
                         "single-turn playback is unavailable");
  }
  std::uint64_t deadline_ms = 0U;
  if (!MakeDeadline(timeout_ms, &deadline_ms)) {
    return Status::Error(StatusCode::kInvalidArgument,
                         "single-turn drain timeout is invalid");
  }
  snd_pcm_t* const playback = Handle(playback_handle_);
  for (;;) {
    const int result = snd_pcm_drain(playback);
    if (result == 0) {
      return Status::Ok();
    }
    if (result != -EAGAIN && result != -EINTR) {
      return AlsaError("ALSA playback drain", result);
    }
    const Status wait_status =
        WaitForPcm(playback, deadline_ms, "ALSA playback drain wait");
    if (!wait_status.ok()) {
      // alsa-lib 1.2.8 reports POLLERR/-EIO when a nonblocking hardware PCM
      // finishes draining and transitions from DRAINING to SETUP. SETUP here
      // means every pending playback frame was consumed, not an xrun.
      if (snd_pcm_state(playback) == SND_PCM_STATE_SETUP) {
        return Status::Ok();
      }
      return wait_status;
    }
  }
}

void AlsaSingleTurnIo::Abort() noexcept {
  if (capture_handle_ != nullptr) {
    static_cast<void>(snd_pcm_drop(Handle(capture_handle_)));
    static_cast<void>(snd_pcm_close(Handle(capture_handle_)));
    capture_handle_ = nullptr;
  }
  if (playback_handle_ != nullptr) {
    static_cast<void>(snd_pcm_drop(Handle(playback_handle_)));
    static_cast<void>(snd_pcm_close(Handle(playback_handle_)));
    playback_handle_ = nullptr;
  }
  capture_started_ = false;
  capture_finished_ = false;
  playback_interleaved_scratch_.fill(0);
}

bool AlsaSingleTurnIo::is_open() const noexcept {
  return capture_handle_ != nullptr && playback_handle_ != nullptr;
}

}  // namespace boompi::platform::rv1106
