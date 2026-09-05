#pragma once

#include <atomic>

#include "boompi/network/voice_link.h"

namespace boompi::network::detail {

/**
 * @brief 建好网卡后确定 WSS 端点；仅由 VoiceLink 网络线程调用。
 * @return 找到格式有效的端点时返回 true，TLS 身份验证留给后续握手。
 */
bool FindServer(const LinkConfig& configured, LinkConfig* found, const std::atomic<bool>* stop);

}  // namespace boompi::network::detail
