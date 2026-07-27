#ifndef BOOMPI_PLATFORM_RV1106_SNOWBOY_LEGACY_BRIDGE_H_
#define BOOMPI_PLATFORM_RV1106_SNOWBOY_LEGACY_BRIDGE_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct BoompiSnowboyLegacyHandle BoompiSnowboyLegacyHandle;

typedef enum BoompiSnowboyLegacyStatus {
  BOOMPI_SNOWBOY_LEGACY_OK = 0,
  BOOMPI_SNOWBOY_LEGACY_INVALID_ARGUMENT = 1,
  BOOMPI_SNOWBOY_LEGACY_ALLOCATION_FAILED = 2,
  BOOMPI_SNOWBOY_LEGACY_BACKEND_EXCEPTION = 3,
  BOOMPI_SNOWBOY_LEGACY_BACKEND_REJECTED = 4,
} BoompiSnowboyLegacyStatus;

typedef struct BoompiSnowboyLegacyInfo {
  uint32_t sample_rate_hz;
  uint16_t channels;
  uint16_t bits_per_sample;
  uint16_t keyword_count;
} BoompiSnowboyLegacyInfo;

BoompiSnowboyLegacyStatus boompi_snowboy_legacy_create(
    const char* resource_path, const char* model_path,
    const char* sensitivity, float audio_gain, int apply_frontend,
    BoompiSnowboyLegacyHandle** handle,
    BoompiSnowboyLegacyInfo* info);

void boompi_snowboy_legacy_destroy(
    BoompiSnowboyLegacyHandle* handle);

BoompiSnowboyLegacyStatus boompi_snowboy_legacy_reset(
    BoompiSnowboyLegacyHandle* handle, int* reset);

BoompiSnowboyLegacyStatus boompi_snowboy_legacy_process_s16(
    BoompiSnowboyLegacyHandle* handle, const int16_t* samples,
    uint32_t sample_count, int32_t* detection_result);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // BOOMPI_PLATFORM_RV1106_SNOWBOY_LEGACY_BRIDGE_H_
