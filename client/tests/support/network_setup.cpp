/**
 * @file network_setup.cpp
 * @brief Host 测试只替换板端网卡与配置文件，保留真实 TLS 和线协议。
 */
#include "network_setup.h"

#include "boompi/config/voice_client_config.h"

namespace boompi::network {

bool SaveWifi(const std::string& ssid, const std::string& password) {
  (void)ssid;
  (void)password;
  return false;
}

bool detail::FindServer(const LinkConfig& configured, LinkConfig* found,
                        const std::atomic<bool>* stop) {
  (void)stop;
  if (found == nullptr) {
    return false;
  }
  *found = configured;
  return !found->host.empty() && found->port != 0 && config::IsValidSpkiSha256(found->spki);
}

}  // namespace boompi::network
