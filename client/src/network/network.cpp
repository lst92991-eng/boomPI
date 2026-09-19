/** @file network.cpp
 * @brief 在各已联网子网发出UDP发现，按已保存公钥选择配套服务端。
 * DHCP、Wi-Fi和路由由系统配置；首次配对在教师准备的课堂网络内完成。
 */
#include "network.h"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <utility>

namespace network
{
/** @brief 在每个UP广播接口所在子网查询，系统路由负责选择实际出口。 */
static bool query(int fd)
{
    ifaddrs *interfaces = nullptr;
    if (getifaddrs(&interfaces) != 0)
    {
        return false;
    }
    bool sent = false;
    const char request[] = "BOOMPI_DISCOVER_V2";
    for (auto *entry = interfaces; entry; entry = entry->ifa_next)
    {
        if (!entry->ifa_addr || !entry->ifa_broadaddr ||
            entry->ifa_addr->sa_family != AF_INET ||
            (entry->ifa_flags & (IFF_UP | IFF_BROADCAST)) != (IFF_UP | IFF_BROADCAST))
        {
            continue;
        }
        auto target = *reinterpret_cast<sockaddr_in *>(entry->ifa_broadaddr);
        target.sin_port = htons(17807);
        const auto count = sendto(fd, request, sizeof(request) - 1, 0,
                                  reinterpret_cast<sockaddr *>(&target), sizeof(target));
        sent = count == static_cast<ssize_t>(sizeof(request) - 1) || sent;
    }
    freeifaddrs(interfaces);
    return sent;
}

/** @brief 等待合法响应；已配对时继续忽略其他服务端，保持原有信任身份。 */
static bool discover(config::VoiceClientConfig &candidate, const std::atomic<bool> &stop)
{
    const int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
    {
        return false;
    }
    const int enabled = 1;
    bool found = false;
    unsigned port = 0;
    char pin[45]{}, address[INET_ADDRSTRLEN]{};
    if (setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &enabled, sizeof(enabled)) == 0 && query(fd))
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(800);
        pollfd readable{fd, POLLIN, 0};
        while (!stop.load() && std::chrono::steady_clock::now() < deadline)
        {
            const int ready = poll(&readable, 1, 50);
            if (ready < 0 && errno != EINTR)
            {
                break;
            }
            if (ready <= 0 || !(readable.revents & POLLIN))
            {
                continue;
            }
            char response[96]{};
            sockaddr_in peer{};
            socklen_t size = sizeof(peer);
            const int count = static_cast<int>(
                recvfrom(fd, response, sizeof(response) - 1, MSG_DONTWAIT | MSG_TRUNC,
                         reinterpret_cast<sockaddr *>(&peer), &size));
            if (count <= 0 || count >= static_cast<int>(sizeof(response)) ||
                std::strlen(response) != static_cast<std::size_t>(count) ||
                peer.sin_port != htons(17807))
            {
                continue;
            }
            char extra;
            if (std::sscanf(response, "BOOMPI_SERVER_V2 %u %44s%c", &port, pin, &extra) != 2 ||
                port == 0 || port > 65535 || !config::IsValidSpkiSha256(pin) ||
                (!candidate.server_spki_sha256.empty() &&
                 candidate.server_spki_sha256 != pin) ||
                !inet_ntop(AF_INET, &peer.sin_addr, address, sizeof(address)))
            {
                continue;
            }
            found = true;
            break;
        }
    }
    ::close(fd);
    if (found && !stop.load())
    {
        // socket先关闭，再把地址交给字符串对象，分配失败时也已归还系统资源。
        candidate.server_ip = address;
        candidate.server_port = static_cast<std::uint16_t>(port);
        candidate.server_spki_sha256 = pin;
    }
    return found && !stop.load();
}

bool find_server(config::VoiceClientConfig &settings, const std::atomic<bool> &stop)
{
    if (stop.load())
    {
        return false;
    }
    auto candidate = settings;
    if (discover(candidate, stop))
    {
        const bool changed = candidate.server_ip != settings.server_ip ||
                             candidate.server_port != settings.server_port ||
                             candidate.server_spki_sha256 != settings.server_spki_sha256;
        // 首次配对先落盘再连接；地址未变化时沿用配置，减少闪存写入。
        if (changed && !config::SaveClientConfig(candidate))
        {
            return false;
        }
        settings = std::move(candidate);
    }
    return !stop.load() && !settings.server_ip.empty();
}
}  // namespace network
