#pragma once
#include <atomic>
#include <cstdint>
#include "boompi/config/voice_client_config.h"

namespace boompi::network {
struct Endpoint {
  config::VoiceClientConfig server;
  const char* interface{nullptr};  // 固定板级名字；Host替身使用nullptr。
};
// 只在网络线程执行：有线优先，失败后无线；显式地址或UDP发现均绑定所选接口。
bool find_server(const config::VoiceClientConfig& config, Endpoint& output,
                 const std::atomic<bool>& stop, bool wifi_first = false);
bool bind_socket(std::intptr_t descriptor, const char* interface) noexcept;
}  // namespace boompi::network
