#pragma once

/**
 * @file network_setup.h
 * @brief voice_net 私有的板端网络准备接口。
 *
 * 产品实现操作 Linux 网卡、DHCP、发现和缓存；Host 测试由 CMake 选择替身实现，
 * 无须在业务协议代码中加入平台或测试条件分支。
 */

#include <atomic>

#include "boompi/network/voice_net.h"

namespace boompi::voice_net::detail {

/**
 * @brief 建好网卡后确定 WSS 端点；仅由 voice_net 网络线程调用。
 *
 * 顺序为以太网优先/Wi-Fi 备用 → 显式端点或 UDP 发现 → 校验已保存 pin → 缓存回退。
 * 本函数可能等待外部命令和 UDP 超时，不得从 ALSA 实时线程调用。
 * @param configured 启动时复制的配置；server_ip 为空表示走自动发现。
 * @param found 非空输出指针；成功时提供 server_ip、server_port、server_spki_sha256，device_id
 * 仍由 configured 持有。
 * @param stop 可选退出标志，在网卡工具等待中周期检查；单次 UDP 接收有 800 ms 超时。
 * @return 找到格式有效的端点时返回 true，TLS 身份验证留给后续握手。
 *         无网卡、无合法端点、退出请求或配置持久化失败且无旧缓存时返回 false。
 */
bool FindServer(const config::VoiceClientConfig& configured, config::VoiceClientConfig* found,
                const std::atomic<bool>* stop);

}  // namespace boompi::voice_net::detail
