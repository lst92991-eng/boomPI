/**
 * @file network_setup.cpp
 * @brief RV1106 启动阶段的网卡建链、UDP 服务发现与配置持久化。
 *
 * 文件中的系统命令、DHCP 等待和配置写入都可能阻塞，只允许在应用启动线程
 * 或配网流程中执行。成功返回后，VoiceLink 的同一网络线程继续TLS/WSS建连。
 */
#include "voice_codec.h"

#include "boompi/config/voice_client_config.h"

#if defined(BOOMPI_TARGET_RV1106)
#include <arpa/inet.h>
#include <fcntl.h>
#include <net/if.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <fstream>
#endif

namespace boompi::network { namespace {

#if defined(BOOMPI_TARGET_RV1106)

/// 子进程等待每 100 ms 检查一次该标志，让关闭流程无需等待 DHCP 自然超时。
bool StopRequested(const std::atomic<bool>* stop) {
  return stop != nullptr && stop->load(std::memory_order_acquire);
}

// 教学板固定使用 eth0/wlan0；服务端缓存留在 /userdata，刷写应用不会覆盖已确认端点。
constexpr char kEthernet[] = "eth0", kWifi[] = "wlan0", kWifiConfig[] = "/etc/wpa_supplicant.conf";
constexpr char kConfigDir[] = "/userdata/boompi/config", kServerConfig[] = "/userdata/boompi/config/server.conf";

/// SSID/密码只接受可打印文本，避免换行或控制字符改变 wpa_supplicant 文件结构。
bool IsText(const std::string& text, std::size_t minimum, std::size_t maximum) {
  if (text.size() < minimum || text.size() > maximum) return false;
  for (const unsigned char byte : text) if (byte < 0x20U || byte == 0x7FU) return false;
  return true;
}

/// 当前只支持 IPv4；SPKI 必须具有标准 SHA-256 Base64 形状才能进入 TLS 阶段。
bool IsEndpoint(const LinkConfig& server) {
  in_addr address{};
  return server.port != 0U &&
         config::IsValidSpkiSha256(server.spki) &&
         inet_pton(AF_INET, server.host.c_str(), &address) == 1;
}

/**
 * @brief 以“临时文件 -> fsync -> rename”原子替换敏感配置。
 *
 * 0600 权限限制普通用户读取 Wi-Fi 密码和服务端 pin；O_NOFOLLOW 防止目标临时
 * 路径被符号链接劫持。写入或同步失败时保留旧文件，并清理未完成的临时文件。
 */
bool AtomicWrite(const char* path, const std::string& text) {
  const std::string temporary = std::string(path) + ".tmp";
  const int fd = open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
  if (fd < 0 || fchmod(fd, 0600) != 0) { if (fd >= 0) { close(fd); unlink(temporary.c_str()); } return false; }
  bool ok = true; std::size_t offset = 0;
  while (ok && offset < text.size()) {
    const ssize_t count = write(fd, text.data() + offset, text.size() - offset);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) { ok = false; } else { offset += static_cast<std::size_t>(count); }
  }
  ok = ok && fsync(fd) == 0; if (close(fd) != 0) { ok = false; }
  if (ok) ok = rename(temporary.c_str(), path) == 0;
  if (!ok) unlink(temporary.c_str());
  return ok;
}

/// 从 sysfs 读取 carrier/operstate 等单值状态，避免为简单探测启动外部工具。
bool NetValueIs(const char* interface, const char* item, const char* expected) {
  std::ifstream input(std::string("/sys/class/net/") + interface + "/" + item);
  std::string value; return (input >> value) && value == expected;
}

/// SIOCGIFADDR 只回答接口当前是否已有 IPv4；链路载波由调用方单独判断。
bool HasIpv4(const char* interface) {
  const int fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) return false;
  ifreq request{}; std::snprintf(request.ifr_name, sizeof(request.ifr_name), "%s", interface);
  const bool found = ioctl(fd, SIOCGIFADDR, &request) == 0; close(fd); return found;
}

/**
 * @brief 有界执行 wpa_supplicant 或 udhcpc，并响应应用退出。
 *
 * wpa_supplicant 使用 -B 将常驻部分留在系统中；udhcpc 最多尝试三次。父进程
 * 每 100 ms 回收子进程，12 秒仍未结束或收到 stop 时强制终止，防止客户端
 * 关闭被外部网络工具无限拖住。标准输出被丢弃，避免凭据或工具噪声进入日志。
 */
bool RunNetworkTool(bool start_wifi, const char* interface,
                    const std::atomic<bool>* stop) {
  const pid_t child = fork();
  if (child == 0) {
    const int null_fd = open("/dev/null", O_WRONLY | O_CLOEXEC);
    if (null_fd >= 0) { dup2(null_fd, STDOUT_FILENO); dup2(null_fd, STDERR_FILENO); }
    if (start_wifi)
      execl("/usr/bin/wpa_supplicant", "wpa_supplicant", "-B", "-i", interface,
            "-c", kWifiConfig, static_cast<char*>(nullptr));
    else
      execl("/sbin/udhcpc", "udhcpc", "-n", "-q", "-t", "3", "-T", "2",
            "-i", interface, static_cast<char*>(nullptr));
    _exit(127);
  }
  if (child < 0) return false;
  int status = 0;
  for (std::uint32_t waited = 0; waited <= 12000U; waited += 100U) {
    if (StopRequested(stop)) {
      kill(child, SIGKILL); waitpid(child, &status, 0); return false;
    }
    const pid_t result = waitpid(child, &status, WNOHANG);
    if (result == child) return WIFEXITED(status) && WEXITSTATUS(status) == 0;
    if (result < 0 && errno != EINTR) return false;
    usleep(100000U);
  }
  kill(child, SIGKILL); waitpid(child, &status, 0); return false;
}

/// 缓存文件必须恰好包含 host、port、SPKI 三项，多余字段按损坏配置处理。
bool LoadServer(LinkConfig* output) {
  std::ifstream input(kServerConfig); unsigned port = 0; std::string extra;
  if (!(input >> output->host >> port >> output->spki) ||
      (input >> extra) || port > 65535U) return false;
  output->port = static_cast<std::uint16_t>(port); return IsEndpoint(*output);
}

/**
 * @brief 在指定网卡上发送固定 v2 广播并解析一个服务端提示。
 *
 * SO_BINDTODEVICE 保证请求和响应走已选中的 eth0 或 wlan0。响应源地址成为 WSS
 * host，报文只携带端口和 SPKI。UDP 无认证能力，此处仅校验格式；真正的服务端
 * 身份在 VoiceLink 的 TLS 握手中通过 SPKI 摘要确认。
 */
bool Discover(const char* interface, LinkConfig* output) {
  const int fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) return false;
  const int enabled = 1; timeval timeout{0, 800000};
  bool ok = setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &enabled, sizeof(enabled)) == 0 &&
            setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0 &&
            setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, interface,
                       std::char_traits<char>::length(interface) + 1U) == 0;
  sockaddr_in target{}; target.sin_family = AF_INET;
  target.sin_port = htons(17807); target.sin_addr.s_addr = INADDR_BROADCAST;
  constexpr char request[] = "BOOMPI_DISCOVER_V2";
  if (ok) ok = sendto(fd, request, sizeof(request) - 1U, 0,
                      reinterpret_cast<sockaddr*>(&target), sizeof(target)) >= 0;
  char response[96]{}; sockaddr_in peer{}; socklen_t peer_size = sizeof(peer);
  const ssize_t bytes = ok ? recvfrom(fd, response, sizeof(response) - 1U, 0,
                                      reinterpret_cast<sockaddr*>(&peer), &peer_size) : -1;
  close(fd);
  if (bytes <= 0 || peer.sin_port != htons(17807)) return false;
  unsigned port = 0; char spki[45]{}, extra = '\0'; response[bytes] = '\0';
  if (std::sscanf(response, "BOOMPI_SERVER_V2 %u %44s%c", &port, spki, &extra) != 2 ||
      port == 0U || port > 65535U) return false;
  char host[INET_ADDRSTRLEN]{};
  if (inet_ntop(AF_INET, &peer.sin_addr, host, sizeof(host)) == nullptr) return false;
  output->host = host; output->port = static_cast<std::uint16_t>(port);
  output->spki = spki; return IsEndpoint(*output);
}

bool SaveServer(const LinkConfig& server) {
  if (!IsEndpoint(server) || (mkdir(kConfigDir, 0700) != 0 && errno != EEXIST)) return false;
  return AtomicWrite(kServerConfig, server.host + " " + std::to_string(server.port) + " " + server.spki + "\n");
}
#endif

}  // namespace

bool SaveWifi(const std::string& ssid, const std::string& password) {
#if defined(BOOMPI_TARGET_RV1106)
  if (!IsText(ssid, 1U, 32U) || !IsText(password, 8U, 63U)) return false;
  auto quote = [](const std::string& input) { std::string output;
    for (const char byte : input) { if (byte == '\\' || byte == '"') { output += '\\'; } output += byte; }
    return output; };
  // 转义后的凭据只写入 0600 文件；调用链也不能把原始 SSID/密码写入普通日志。
  return AtomicWrite(kWifiConfig, "# boomPI-managed\nctrl_interface=/var/run/wpa_supplicant\nnetwork={\n  ssid=\"" +
                     quote(ssid) + "\"\n  psk=\"" + quote(password) + "\"\n}\n");
#else
  (void)ssid;
  (void)password;
  return false;
#endif
}

bool detail::FindServer(const LinkConfig& configured, LinkConfig* output,
                         const std::atomic<bool>* stop) {
  if (output == nullptr) return false;
#if defined(BOOMPI_TARGET_RV1106)
  *output = {}; const char* selected = nullptr;
  // 以太网优先：有载波且已有地址时直接复用；仅在缺少地址时运行一次有界 DHCP。
  // 有线无法取得 IPv4 才进入 Wi-Fi 分支，确保插网线时的路径稳定且延迟最低。
  if (NetValueIs(kEthernet, "carrier", "1") &&
      (HasIpv4(kEthernet) ||
       (RunNetworkTool(false, kEthernet, stop) && HasIpv4(kEthernet)))) {
    selected = kEthernet;
  } else {
    // Wi-Fi 是备用路径，必须先有配网页写入的 wpa_supplicant 配置。接口尚未
    // 建链时先启动 supplicant，再通过同一个有界 udhcpc 流程获取 IPv4。
    if (access("/sys/class/net/wlan0", F_OK) != 0 || access(kWifiConfig, R_OK) != 0) return false;
    if (!HasIpv4(kWifi) &&
        ((!NetValueIs(kWifi, "operstate", "up") &&
          !RunNetworkTool(true, kWifi, stop)) ||
         !RunNetworkTool(false, kWifi, stop) || !HasIpv4(kWifi))) return false;
    selected = kWifi;
  }
  if (StopRequested(stop)) return false;
  // 显式地址只替代服务器查找；真实网卡和路由仍需先建立，随后直接交给 TLS。
  if (!configured.host.empty()) {
    if (!IsEndpoint(configured)) return false;
    *output = configured;
    return true;
  }
  // 发现结果与已保存 SPKI 一致时才更新地址，使 DHCP 导致的服务器 IP 变化可以
  // 自动恢复，同时阻止同一局域网里的陌生广播静默替换已经确认的服务端公钥。
  // 没收到广播时继续使用缓存；连接可达性和 pin 校验由后续 WSS 建连给出结论。
  LinkConfig saved{}, found{}; const bool have_saved = LoadServer(&saved);
  if (Discover(selected, &found) &&
      (!have_saved || found.spki == saved.spki) && SaveServer(found)) {
    *output = found; return true;
  }
  if (!have_saved) return false;
  *output = saved; return true;
#else
  // Host loopback exercises the production TLS/protocol path without touching NICs.
  (void)stop;
  *output = configured;
  return !output->host.empty() && output->port != 0 && config::IsValidSpkiSha256(output->spki);
#endif
}

}  // namespace boompi::network
