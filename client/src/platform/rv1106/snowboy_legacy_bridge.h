/// @file 隔离 Snowboy 旧 libstdc++ ABI 的最小 C 接口。
#pragma once

#include <stdint.h>

extern "C" {

/// @brief Snowboy 旧 C++ ABI 句柄。
///
/// 该边界只传递 POD、整数和不透明指针，使 bridge 可以单独使用 Snowboy 所需的
/// `_GLIBCXX_USE_CXX11_ABI=0`，主客户端继续使用正常 C++17 ABI。
typedef struct BoompiSnowboyLegacyHandle BoompiSnowboyLegacyHandle;

/// @brief 创建并校验 16 kHz/S16/mono Snowboy 检测器。
/// @return 成功返回 1 并写出句柄；参数、模型或异常错误返回 0，输出句柄保持为空。
int boompi_snowboy_legacy_create(const char* resource_path, const char* model_path,
                                 const char* sensitivity, float audio_gain,
                                 BoompiSnowboyLegacyHandle** handle);
/// @brief 销毁句柄；接受空指针，任何 C++ 异常都被限制在 ABI 边界内。
void boompi_snowboy_legacy_destroy(BoompiSnowboyLegacyHandle* handle);
/// @brief 清空 Snowboy 内部检测历史，用于会话状态切换。
int boompi_snowboy_legacy_reset(BoompiSnowboyLegacyHandle* handle);
/// @brief 处理一段 16 kHz/S16/mono PCM，检测结果沿用 Snowboy 的整数语义。
/// @return API 调用成功返回 1；参数或内部异常返回 0，并把可用结果清零。
int boompi_snowboy_legacy_process_s16(BoompiSnowboyLegacyHandle* handle, const int16_t* samples,
                                      uint32_t sample_count, int32_t* detection_result);
}
