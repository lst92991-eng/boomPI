#pragma once

#include <stdint.h>

extern "C" {

// Snowboy 使用旧 libstdc++ ABI；主程序只跨越 POD、整数和不透明指针。
typedef struct BoompiSnowboyLegacyHandle BoompiSnowboyLegacyHandle;

int boompi_snowboy_legacy_create(const char* resource_path,
                                 const char* model_path,
                                 const char* sensitivity, float audio_gain,
                                 BoompiSnowboyLegacyHandle** handle);
void boompi_snowboy_legacy_destroy(BoompiSnowboyLegacyHandle* handle);
int boompi_snowboy_legacy_reset(BoompiSnowboyLegacyHandle* handle);
int boompi_snowboy_legacy_process_s16(BoompiSnowboyLegacyHandle* handle,
                                      const int16_t* samples,
                                      uint32_t sample_count,
                                      int32_t* detection_result);

}
