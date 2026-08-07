#pragma once

/**
 * @file voice_client.h
 * @brief 暴露板端语音 application 的唯一进程级入口。
 *
 * main 只需提供经过校验的只读配置和停止标志。具体状态机、线程协作以及资源释放
 * 保留在实现文件中，其他模块无法绕过 actor 直接修改会话状态。
 */

#include <csignal>
#include <string>

#include "boompi/config/voice_client_config.h"

namespace boompi::application {

/**
 * @brief 运行板端语音应用，直到收到退出信号或出现不可恢复错误。
 *
 * 调用线程就是唯一的业务 actor：它串行推进对话状态机、采集 20 ms 音频帧并发送
 * 上行数据。WSS、capture、playback 和 UI 线程通过有界队列或小型快照与它通信，
 * 会话状态始终只有一个写入者。
 *
 * @param config 启动阶段完成校验、运行期间保持只读的板端配置。
 * @param stop 可选停止标志；信号处理器只需将其置位，函数会在正常线程中有序退出。
 * @param error 可选错误输出；失败时返回适合普通日志的阶段说明。
 * @return 正常停止时返回 true，初始化或运行中出现不可恢复错误时返回 false。
 */
bool RunVoiceClient(const config::VoiceClientConfig& config,
                    const volatile std::sig_atomic_t* stop,
                    std::string* error);

}  // namespace boompi::application
