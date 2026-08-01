#ifndef BOOMPI_PLATFORM_RV1106_SNOWBOY_LEGACY_BRIDGE_H_
#define BOOMPI_PLATFORM_RV1106_SNOWBOY_LEGACY_BRIDGE_H_

/// @file
/// @brief Snowboy 旧 libstdc++ ABI 的 C 边界。
///
/// 本头只暴露 POD、整数和裸指针，防止旧 `std::string` ABI 泄漏到主程序。每个 handle
/// 必须由调用方串行访问；桥接层不提供并发保护。

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief 由 create 创建、由 destroy 释放的调用方所有不透明句柄。
typedef struct BoompiSnowboyLegacyHandle BoompiSnowboyLegacyHandle;

/// @brief C ABI 调用结果。
typedef enum BoompiSnowboyLegacyStatus {
  BOOMPI_SNOWBOY_LEGACY_OK = 0,                 ///< 桥接调用完成。
  BOOMPI_SNOWBOY_LEGACY_INVALID_ARGUMENT = 1,   ///< 空指针、空字符串或非法样本数。
  BOOMPI_SNOWBOY_LEGACY_ALLOCATION_FAILED = 2,  ///< C++ 对象或句柄分配失败。
  BOOMPI_SNOWBOY_LEGACY_BACKEND_EXCEPTION = 3,  ///< Snowboy 抛出异常，已在 C 边界截获。
  BOOMPI_SNOWBOY_LEGACY_BACKEND_REJECTED = 4,   ///< Snowboy 元数据或 Reset 结果不满足契约。
} BoompiSnowboyLegacyStatus;

/// @brief 创建成功后返回的模型输入契约。
typedef struct BoompiSnowboyLegacyInfo {
  uint32_t sample_rate_hz;  ///< 模型要求的每秒采样数。
  uint16_t channels;        ///< 模型要求的 PCM 通道数。
  uint16_t bits_per_sample; ///< 每个 PCM sample 的有效位数。
  uint16_t keyword_count;   ///< 模型内关键词数量。
} BoompiSnowboyLegacyInfo;

/// @brief 创建检测器并返回调用方拥有的不透明句柄。
///
/// 路径和 sensitivity 必须是本次调用期间有效的非空 NUL 结尾字符串；`audio_gain` 是 Snowboy
/// 线性输入增益，`apply_frontend != 0` 表示启用其自带前端。任何 C++ 异常都转换为状态码。
///
/// @param handle 成功时接收句柄，并必须交给 destroy。只有返回 OK 时输出有效。
/// @param info 成功时接收模型采样率、通道、位宽和关键词数量；只有返回 OK 时输出有效。
/// @return 创建结果；失败时调用方不得读取未由成功路径写入的输出。
BoompiSnowboyLegacyStatus boompi_snowboy_legacy_create(
    const char* resource_path, const char* model_path,
    const char* sensitivity, float audio_gain, int apply_frontend,
    BoompiSnowboyLegacyHandle** handle,
    BoompiSnowboyLegacyInfo* info);

/// @brief 销毁句柄；允许空指针，返回后原句柄失效，异常不会穿过 C ABI。
void boompi_snowboy_legacy_destroy(
    BoompiSnowboyLegacyHandle* handle);

/// @brief 清空检测器内部历史。
///
/// handle 必须有效且 `reset` 必须非空。返回 OK 时写入 1；Snowboy 返回 false 时写入 0 并
/// 返回 BACKEND_REJECTED。
BoompiSnowboyLegacyStatus boompi_snowboy_legacy_reset(
    BoompiSnowboyLegacyHandle* handle, int* reset);

/// @brief 处理一段 16-bit PCM；格式必须匹配 create 返回的模型信息。
///
/// `sample_count` 的单位是 int16 sample，不是字节；本产品传入 16 kHz mono 的 320 samples。
/// 参数有效时先把 `detection_result` 清零；返回 OK 后，大于零表示关键词索引，其余检测结果
/// 按 Snowboy 原值传回。桥接状态只说明调用是否完成，不等同于关键词命中。
BoompiSnowboyLegacyStatus boompi_snowboy_legacy_process_s16(
    BoompiSnowboyLegacyHandle* handle, const int16_t* samples,
    uint32_t sample_count, int32_t* detection_result);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // BOOMPI_PLATFORM_RV1106_SNOWBOY_LEGACY_BRIDGE_H_
