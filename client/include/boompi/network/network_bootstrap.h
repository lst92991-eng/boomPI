#pragma once

#include <cstdint>
#include <string>

namespace boompi::network {

struct NetworkBootstrapResult final {
  std::string host;
  std::uint16_t port{0};
  std::string spki_sha256_base64;
};

// 教学版只保留“存配置、选网络、找服务端”三步。
class NetworkBootstrap final {
 public:
  static bool SaveWifi(const std::string& ssid, const std::string& password);
  static bool SaveServer(const NetworkBootstrapResult& server);
  static bool Start(bool enable_discovery, NetworkBootstrapResult* output);
};

}  // namespace boompi::network
