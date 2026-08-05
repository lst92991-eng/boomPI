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
  /// 始终先完成有线/Wi-Fi 建链。explicit_server 非空时直接使用，不重复写入
  /// discovery 缓存；否则按“局域网发现 -> 已保存地址”回退。stop 可中断 DHCP。
  static bool Start(const NetworkBootstrapResult* explicit_server,
                    NetworkBootstrapResult* output,
                    const std::atomic<bool>* stop = nullptr);
};

}  // namespace boompi::network
