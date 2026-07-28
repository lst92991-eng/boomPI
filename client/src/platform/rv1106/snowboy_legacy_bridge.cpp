#include "snowboy_legacy_bridge.h"

#include <climits>
#include <cstdint>
#include <new>
#include <string>

#include "snowboy-detect.h"

struct BoompiSnowboyLegacyHandle final {
  snowboy::SnowboyDetect* detector;
};

BoompiSnowboyLegacyStatus boompi_snowboy_legacy_create(
    const char* const resource_path, const char* const model_path,
    const char* const sensitivity, const float audio_gain,
    const int apply_frontend, BoompiSnowboyLegacyHandle** const handle,
    BoompiSnowboyLegacyInfo* const info) {
  if (resource_path == nullptr || resource_path[0] == '\0' ||
      model_path == nullptr || model_path[0] == '\0' ||
      sensitivity == nullptr || sensitivity[0] == '\0' ||
      handle == nullptr || info == nullptr) {
    return BOOMPI_SNOWBOY_LEGACY_INVALID_ARGUMENT;
  }

  *handle = nullptr;
  *info = BoompiSnowboyLegacyInfo{};
  try {
    auto* detector =
        new snowboy::SnowboyDetect(std::string(resource_path),
                                  std::string(model_path));
    auto* legacy_handle =
        new (std::nothrow) BoompiSnowboyLegacyHandle{detector};
    if (legacy_handle == nullptr) {
      delete detector;
      return BOOMPI_SNOWBOY_LEGACY_ALLOCATION_FAILED;
    }

    try {
      detector->SetSensitivity(std::string(sensitivity));
      detector->SetAudioGain(audio_gain);
      detector->ApplyFrontend(apply_frontend != 0);

      const int sample_rate_hz = detector->SampleRate();
      const int channels = detector->NumChannels();
      const int bits_per_sample = detector->BitsPerSample();
      const int keyword_count = detector->NumHotwords();
      if (sample_rate_hz <= 0 || channels <= 0 || bits_per_sample <= 0 ||
          keyword_count <= 0 || channels > UINT16_MAX ||
          bits_per_sample > UINT16_MAX ||
          keyword_count > UINT16_MAX) {
        delete legacy_handle->detector;
        delete legacy_handle;
        return BOOMPI_SNOWBOY_LEGACY_BACKEND_REJECTED;
      }

      info->sample_rate_hz = static_cast<std::uint32_t>(sample_rate_hz);
      info->channels = static_cast<std::uint16_t>(channels);
      info->bits_per_sample =
          static_cast<std::uint16_t>(bits_per_sample);
      info->keyword_count =
          static_cast<std::uint16_t>(keyword_count);
      *handle = legacy_handle;
      return BOOMPI_SNOWBOY_LEGACY_OK;
    } catch (...) {
      delete legacy_handle->detector;
      delete legacy_handle;
      return BOOMPI_SNOWBOY_LEGACY_BACKEND_EXCEPTION;
    }
  } catch (const std::bad_alloc&) {
    return BOOMPI_SNOWBOY_LEGACY_ALLOCATION_FAILED;
  } catch (...) {
    return BOOMPI_SNOWBOY_LEGACY_BACKEND_EXCEPTION;
  }
}

void boompi_snowboy_legacy_destroy(
    BoompiSnowboyLegacyHandle* const handle) {
  if (handle == nullptr) {
    return;
  }
  try {
    delete handle->detector;
    delete handle;
  } catch (...) {
    // Never allow a third-party destructor to unwind across the C boundary.
  }
}

BoompiSnowboyLegacyStatus boompi_snowboy_legacy_reset(
    BoompiSnowboyLegacyHandle* const handle, int* const reset) {
  if (handle == nullptr || handle->detector == nullptr || reset == nullptr) {
    return BOOMPI_SNOWBOY_LEGACY_INVALID_ARGUMENT;
  }
  *reset = 0;
  try {
    *reset = handle->detector->Reset() ? 1 : 0;
    return *reset != 0 ? BOOMPI_SNOWBOY_LEGACY_OK
                       : BOOMPI_SNOWBOY_LEGACY_BACKEND_REJECTED;
  } catch (...) {
    return BOOMPI_SNOWBOY_LEGACY_BACKEND_EXCEPTION;
  }
}

BoompiSnowboyLegacyStatus boompi_snowboy_legacy_process_s16(
    BoompiSnowboyLegacyHandle* const handle,
    const int16_t* const samples, const uint32_t sample_count,
    int32_t* const detection_result) {
  if (handle == nullptr || handle->detector == nullptr ||
      samples == nullptr || sample_count == 0U ||
      sample_count > static_cast<uint32_t>(INT_MAX) ||
      detection_result == nullptr) {
    return BOOMPI_SNOWBOY_LEGACY_INVALID_ARGUMENT;
  }

  *detection_result = 0;
  try {
    *detection_result = static_cast<int32_t>(
        handle->detector->RunDetection(
            samples, static_cast<int>(sample_count), false));
    return BOOMPI_SNOWBOY_LEGACY_OK;
  } catch (...) {
    return BOOMPI_SNOWBOY_LEGACY_BACKEND_EXCEPTION;
  }
}
