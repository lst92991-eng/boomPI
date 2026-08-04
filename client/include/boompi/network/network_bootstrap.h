#pragma once

#include <atomic>
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
  /// stop 非空时，长阻塞步骤（DHCP 等待、重试间隙）会观察它并提前退出，
  /// 保证进程退出路径不会被最长 12 秒的外部工具等待卡住。
  static bool Start(bool enable_discovery, NetworkBootstrapResult* output,
                    const std::atomic<bool>* stop = nullptr);
};

}  // namespace boompi::network
