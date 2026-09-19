/** @file network.h
 * @brief 在系统已联网的接口上发现配套服务端，更新同一份持久配置。
 */
#pragma once
#include <atomic>

#include "boompi/config/voice_client_config.h"

namespace network
{
// 仅由网络线程调用；匹配已有指纹，发现失败时使用保存地址，stop可打断等待。
bool find_server(config::VoiceClientConfig &settings, const std::atomic<bool> &stop);
}  // namespace network
