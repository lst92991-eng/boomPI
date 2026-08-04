#pragma once

#include <csignal>
#include <string>

#include "boompi/config/voice_client_config.h"

namespace boompi::application {

/// @brief 运行板端语音应用，直到收到退出信号或出现不可恢复错误。
///
/// 调用线程就是唯一的业务 actor：它串行推进对话状态机、采集 20 ms 音频帧并发送
/// 上行数据。WSS 和播放线程只能通过各自的有界队列与它通信，不能直接修改会话状态。
bool RunVoiceClient(const config::VoiceClientConfig& config,
                    const volatile std::sig_atomic_t* stop,
                    std::string* error);

}  // namespace boompi::application
