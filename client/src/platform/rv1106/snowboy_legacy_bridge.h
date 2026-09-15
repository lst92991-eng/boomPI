/**
 * @file snowboy_legacy_bridge.h
 * @brief 隔离 Snowboy 旧 libstdc++ ABI 的最小 C 链接接口。
 *
 * SpeechDetector 在启动时 create，运行时由采集线程顺序 reset/process，退出时 destroy。
 * 指针都只在调用期间借用；调用方不解释句柄布局，也不跨边界传 std::string 或异常。
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Snowboy 旧 C++ ABI 句柄。
///
/// 该边界只传递 POD、整数和不透明指针，使 bridge 可以单独使用 Snowboy 所需的
/// `_GLIBCXX_USE_CXX11_ABI=0`，主客户端继续使用正常 C++17 ABI。
typedef struct BoompiSnowboyLegacyHandle BoompiSnowboyLegacyHandle;

/// @brief 创建并校验 16 kHz/S16/mono Snowboy 检测器。
/// resource_path/model_path 是板端预置资源路径；sensitivity 按 Snowboy 接口使用字符串。
/// 输出槽应先初始化为空；通过参数检查后才会清空输出槽，再尝试加载模型。
/// @return 成功返回 1 并写出句柄；参数、模型或异常错误返回 0，不得使用该次输出。
int boompi_snowboy_legacy_create(const char* resource_path, const char* model_path,
                                 const char* sensitivity, float audio_gain,
                                 BoompiSnowboyLegacyHandle** handle);
/// @brief 销毁句柄；接受空指针，任何 C++ 异常都被限制在 ABI 边界内。
void boompi_snowboy_legacy_destroy(BoompiSnowboyLegacyHandle* handle);
/// @brief 清空 Snowboy 内部检测历史，用于会话状态切换。
/// 采集线程在 period 边界执行；成功返回 1，空句柄、库失败或异常返回 0。
int boompi_snowboy_legacy_reset(BoompiSnowboyLegacyHandle* handle);
/// @brief 处理一段 16 kHz/S16/mono PCM，检测结果沿用 Snowboy 的整数语义。
/// sample_count 为单声道样本数，产品传 320；本接口只检查长度可转换为 int。
/// @return API 调用完成返回 1，检测结果 >0 才表示命中；返回 0 时调用方不得使用结果。
/// 参数检查失败不改输出；只有捕获到库异常的路径将可用结果清零。
int boompi_snowboy_legacy_process_s16(BoompiSnowboyLegacyHandle* handle, const int16_t* samples,
                                      uint32_t sample_count, int32_t* detection_result);
#ifdef __cplusplus
}
#endif
