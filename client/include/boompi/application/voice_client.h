#pragma once

/**
 * @file voice_client.h
 * @brief 启动对话主循环并管理音频、网络和显示的生命周期。
 */

#include <csignal>
#include <string>

#include "boompi/config/voice_client_config.h"

namespace boompi::application {

/**
 * @brief 运行板端语音应用，直到收到退出信号或出现不可恢复错误。
 *
 * 调用线程处理对话状态；其他线程通过音频/网络队列和UI快照交付结果。
 * 函数返回前会停止并回收所有工作线程。
 *
 * @param config 启动阶段完成校验、运行期间保持只读的板端配置。
 * @param stop 可选停止标志；信号处理器只需将其置位，函数会在正常线程中有序退出。
 * @param error 可选错误输出；失败时返回适合普通日志的阶段说明。
 * @return 正常停止时返回 true，初始化或运行中出现不可恢复错误时返回 false。
 */
bool RunVoiceClient(const config::VoiceClientConfig& config,
                    const volatile std::sig_atomic_t* stop, std::string* error);

}  // namespace boompi::application
