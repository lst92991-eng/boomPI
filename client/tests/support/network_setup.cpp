/**
 * @file network_setup.cpp
 * @brief Host 测试只替换板端网卡与配置文件，保留真实 TLS 和线协议。
 *
 * CMake 为传输测试选择本文件，产品构建选择 src/network/network_setup.cpp。
 * 测试提供本机回环 IPv4、临时端口和测试证书 pin，随后仍运行产品 voice_net 的
 * WebSocket++/OpenSSL/hello/PCM/重连链路；不覆盖 eth0/wlan0、DHCP 或 UDP 发现。
 */
#include "network_setup.h"

#include "boompi/config/voice_client_config.h"

namespace boompi::voice_net {

/// @brief 测试环境明确拒绝保存凭据，防止 Host 回归覆盖开发机系统 Wi-Fi 配置。
bool save_wifi(const std::string& ssid, const std::string& password) {
  (void)ssid;
  (void)password;
  return false;
}

/**
 * @brief 直接返回测试显式端点，以确定性输入替代板端网络准备。
 *
 * 此路径没有阻塞工具和广播接收，因此不需要轮询 stop；真正 TLS pin 校验仍在
 * voice_net Connect/VerifyPin 执行，这里检查 pin 格式不等于放行任意服务器。
 */
bool detail::FindServer(const config::VoiceClientConfig& configured,
                        config::VoiceClientConfig* found, const std::atomic<bool>* stop) {
  (void)stop;
  if (found == nullptr) {
    return false;
  }
  *found = configured;
  return !found->server_ip.empty() && found->server_port != 0 &&
         config::IsValidSpkiSha256(found->server_spki_sha256);
}

}  // namespace boompi::voice_net
