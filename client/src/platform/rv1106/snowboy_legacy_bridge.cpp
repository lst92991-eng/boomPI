#include "snowboy_legacy_bridge.h"

#include <climits>
#include <memory>
#include <string>

#include "snowboy-detect.h"

struct BoompiSnowboyLegacyHandle final {
  snowboy::SnowboyDetect* detector;
};

int boompi_snowboy_legacy_create(
    const char* resource, const char* model, const char* sensitivity,
    float gain, BoompiSnowboyLegacyHandle** handle) {
  if (resource == nullptr || *resource == '\0' || model == nullptr ||
      *model == '\0' || sensitivity == nullptr || *sensitivity == '\0' ||
      handle == nullptr) return 0;
  *handle = nullptr;
  try {
    auto detector = std::make_unique<snowboy::SnowboyDetect>(resource, model);
    detector->SetSensitivity(sensitivity); detector->SetAudioGain(gain);
    detector->ApplyFrontend(false);
    if (detector->SampleRate() != 16000 || detector->NumChannels() != 1 ||
        detector->BitsPerSample() != 16 || detector->NumHotwords() < 1) return 0;
    *handle = new BoompiSnowboyLegacyHandle{detector.release()};
    return 1;
  } catch (...) { return 0; }
}

void boompi_snowboy_legacy_destroy(BoompiSnowboyLegacyHandle* handle) {
  if (handle == nullptr) return;
  try { delete handle->detector; } catch (...) {}
  delete handle;
}

int boompi_snowboy_legacy_reset(BoompiSnowboyLegacyHandle* handle) {
  if (handle == nullptr || handle->detector == nullptr) return 0;
  try { return handle->detector->Reset() ? 1 : 0; } catch (...) { return 0; }
}

int boompi_snowboy_legacy_process_s16(
    BoompiSnowboyLegacyHandle* handle, const int16_t* samples,
    uint32_t count, int32_t* result) {
  if (handle == nullptr || handle->detector == nullptr || samples == nullptr ||
      count == 0U || count > static_cast<uint32_t>(INT_MAX) || result == nullptr)
    return 0;
  try {
    *result = handle->detector->RunDetection(samples, static_cast<int>(count), false);
    return 1;
  } catch (...) { *result = 0; return 0; }
}
