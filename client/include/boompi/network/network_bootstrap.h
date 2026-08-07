#pragma once

/**
 * @file network_bootstrap.h
 * @brief 板端启动阶段的网络选择、Wi-Fi 凭据保存和服务端发现入口。
 *
 * 该模块在语音会话建立前完成一次阻塞式建链：优先使用以太网，必要时启用
 * 已配置的 Wi-Fi，然后从显式配置、局域网发现或本地缓存中得到 WSS 端点。
 * 音频实时线程和持久 WSS 的生命周期分别由 audio 与 VoiceTransport 管理。
 */

#include <atomic>
#include <cstdint>
#include <string>

namespace boompi::network {

/**
 * @brief 建立 WSS 所需的完整服务端端点。
 *
 * UDP 发现提供 IPv4 地址、WSS 端口和 SPKI SHA-256。地址负责路由，SPKI
 * 在后续 TLS 握手中确认服务端身份，因此三项必须作为一个整体传递和保存。
 */
struct NetworkBootstrapResult final {
  std::string host;
  std::uint16_t port{0};
  std::string spki_sha256_base64;
};

/**
 * @brief 教学版网络启动流程：“存配置、选网络、找服务端”。
 *
 * 所有接口由启动线程或 UI 配网流程调用，内部不会创建常驻 worker。这样能够
 * 让网卡建链与语音传输保持清晰边界，也避免 DHCP、文件写入进入音频热路径。
 */
class NetworkBootstrap final {
 public:
  /// 校验并原子保存 wpa_supplicant 配置；凭据只落入权限为 0600 的文件。
  static bool SaveWifi(const std::string& ssid, const std::string& password);

  /// 原子保存已确认的服务端地址和 SPKI，供局域网发现失败时回退使用。
  static bool SaveServer(const NetworkBootstrapResult& server);

  /**
   * @brief 建立网络并返回可用于 VoiceTransport 的服务端端点。
   *
   * 始终先完成有线/Wi-Fi 建链。explicit_server 非空时直接使用，不改写发现
   * 缓存；否则按“局域网发现 -> 已保存地址”回退。stop 让应用退出能够中断
   * 最长数秒的 DHCP 子进程等待。
   *
   * @param explicit_server 用户明确指定的端点，可为空。
   * @param output 成功时接收经过格式校验的完整端点。
   * @param stop 可选的跨线程退出标志；本函数只读取它。
   * @return 网卡已建链且获得服务端端点时返回 true。
   */
  static bool Start(const NetworkBootstrapResult* explicit_server,
                    NetworkBootstrapResult* output,
                    const std::atomic<bool>* stop = nullptr);
};

}  // namespace boompi::network
