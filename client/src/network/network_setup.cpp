/**
 * @file network_setup.cpp
 * @brief 教学板的网卡建链、UDP 服务发现和配置保存。
 *
 * FindServer 由 VoiceLink 线程调用；配网页只调用 SaveWifi 保存配置。
 * DHCP、外部命令和文件写入都可能等待，不能放到采集或播放线程。
 */
#include "network_setup.h"

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

#include "boompi/config/voice_client_config.h"

namespace boompi::network {
namespace {

// 网卡名来自教学板系统；已确认的服务器保存在 userdata，更新应用时不覆盖。
constexpr char kEthernet[] = "eth0";
constexpr char kWifi[] = "wlan0";
constexpr char kWifiConfig[] = "/etc/wpa_supplicant.conf";
constexpr char kConfigDir[] = "/userdata/boompi/config";
constexpr char kServerConfig[] = "/userdata/boompi/config/server.conf";

bool StopRequested(const std::atomic<bool>* stop) {
  return stop != nullptr && stop->load(std::memory_order_acquire);
}

bool IsText(const std::string& text, std::size_t minimum, std::size_t maximum) {
  if (text.size() < minimum || text.size() > maximum) {
    return false;
  }
  for (const unsigned char byte : text) {
    if (byte < 0x20U || byte == 0x7FU) {
      return false;
    }
  }
  return true;
}

bool IsEndpoint(const LinkConfig& server) {
  in_addr address{};
  return server.port != 0U && config::IsValidSpkiSha256(server.spki) &&
         inet_pton(AF_INET, server.host.c_str(), &address) == 1;
}

/**
 * @brief 完整写好临时文件后再替换配置，写入失败时保留旧文件。
 *
 * Wi-Fi 密码只允许文件属主读取；O_NOFOLLOW 防止临时路径被符号链接替换。
 * 先 fsync 再 rename，避免读者看到只写了一半的配置。
 */
bool AtomicWrite(const char* path, const std::string& text) {
  const std::string temporary = std::string(path) + ".tmp";
  const int fd =
      open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
  if (fd < 0) {
    return false;
  }
  if (fchmod(fd, 0600) != 0) {
    close(fd);
    unlink(temporary.c_str());
    return false;
  }

  bool succeeded = true;
  std::size_t offset = 0;
  while (offset < text.size()) {
    const ssize_t written = write(fd, text.data() + offset, text.size() - offset);
    if (written < 0 && errno == EINTR) {
      continue;
    }
    if (written <= 0) {
      succeeded = false;
      break;
    }
    offset += static_cast<std::size_t>(written);
  }
  if (succeeded) {
    succeeded = fsync(fd) == 0;
  }
  if (close(fd) != 0) {
    succeeded = false;
  }
  if (succeeded) {
    succeeded = rename(temporary.c_str(), path) == 0;
  }
  if (!succeeded) {
    unlink(temporary.c_str());
  }
  return succeeded;
}

bool NetValueIs(const char* interface, const char* item, const char* expected) {
  std::ifstream input(std::string("/sys/class/net/") + interface + "/" + item);
  std::string value;
  return (input >> value) && value == expected;
}

bool HasIpv4(const char* interface) {
  const int fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) {
    return false;
  }
  ifreq request{};
  std::snprintf(request.ifr_name, sizeof(request.ifr_name), "%s", interface);
  const bool found = ioctl(fd, SIOCGIFADDR, &request) == 0;
  close(fd);
  return found;
}

/**
 * @brief 启动 supplicant 或 DHCP，并在退出请求或超时后回收子进程。
 *
 * supplicant 的 -B 留下系统配网服务，DHCP 最多尝试三次。父进程每 100 ms
 * 检查 stop，最长等 12 秒；工具输出不进入日志，以免泄露凭据。
 */
bool RunNetworkTool(bool start_wifi, const char* interface, const std::atomic<bool>* stop) {
  const pid_t child = fork();
  if (child == 0) {
    const int null_fd = open("/dev/null", O_WRONLY | O_CLOEXEC);
    if (null_fd >= 0) {
      dup2(null_fd, STDOUT_FILENO);
      dup2(null_fd, STDERR_FILENO);
    }
    if (start_wifi) {
      execl("/usr/bin/wpa_supplicant", "wpa_supplicant", "-B", "-i", interface, "-c",
            kWifiConfig, static_cast<char*>(nullptr));
    } else {
      execl("/sbin/udhcpc", "udhcpc", "-n", "-q", "-t", "3", "-T", "2", "-i", interface,
            static_cast<char*>(nullptr));
    }
    _exit(127);
  }
  if (child < 0) {
    return false;
  }

  int status = 0;
  for (std::uint32_t waited_ms = 0; waited_ms <= 12000U; waited_ms += 100U) {
    if (StopRequested(stop)) {
      kill(child, SIGKILL);
      waitpid(child, &status, 0);
      return false;
    }
    const pid_t result = waitpid(child, &status, WNOHANG);
    if (result == child) {
      return WIFEXITED(status) && WEXITSTATUS(status) == 0;
    }
    if (result < 0 && errno != EINTR) {
      return false;
    }
    usleep(100000U);
  }
  kill(child, SIGKILL);
  waitpid(child, &status, 0);
  return false;
}

/// @brief 优先复用有线地址；只有有线不可用时才进入 Wi-Fi 配网流程。
const char* SelectInterface(const std::atomic<bool>* stop) {
  if (NetValueIs(kEthernet, "carrier", "1") &&
      (HasIpv4(kEthernet) || (RunNetworkTool(false, kEthernet, stop) && HasIpv4(kEthernet)))) {
    return kEthernet;
  }

  if (access("/sys/class/net/wlan0", F_OK) != 0 || access(kWifiConfig, R_OK) != 0) {
    return nullptr;
  }
  if (HasIpv4(kWifi)) {
    return kWifi;
  }
  if (!NetValueIs(kWifi, "operstate", "up") && !RunNetworkTool(true, kWifi, stop)) {
    return nullptr;
  }
  if (!RunNetworkTool(false, kWifi, stop) || !HasIpv4(kWifi)) {
    return nullptr;
  }
  return kWifi;
}

bool LoadServer(LinkConfig* output) {
  std::ifstream input(kServerConfig);
  unsigned port = 0;
  std::string extra;
  if (!(input >> output->host >> port >> output->spki) || (input >> extra) || port > 65535U) {
    return false;
  }
  output->port = static_cast<std::uint16_t>(port);
  return IsEndpoint(*output);
}

/**
 * @brief 在选定网卡上广播发现请求，用响应源地址作为服务器地址。
 *
 * SO_BINDTODEVICE 避免请求从另一张网卡发出。UDP 只能提供地址和公钥提示，
 * 已配对设备还要与缓存 SPKI 比对，随后由 TLS 验证持有该公钥的服务器。
 */
bool Discover(const char* interface, LinkConfig* output) {
  const int fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) {
    return false;
  }
  const int enabled = 1;
  timeval timeout{0, 800000};
  bool succeeded = setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &enabled, sizeof(enabled)) == 0 &&
                   setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0 &&
                   setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, interface,
                              std::char_traits<char>::length(interface) + 1U) == 0;

  sockaddr_in target{};
  target.sin_family = AF_INET;
  target.sin_port = htons(17807);
  target.sin_addr.s_addr = INADDR_BROADCAST;
  constexpr char request[] = "BOOMPI_DISCOVER_V2";
  if (succeeded) {
    succeeded = sendto(fd, request, sizeof(request) - 1U, 0,
                       reinterpret_cast<sockaddr*>(&target), sizeof(target)) >= 0;
  }

  char response[96]{};
  sockaddr_in peer{};
  socklen_t peer_size = sizeof(peer);
  ssize_t received = -1;
  if (succeeded) {
    received = recvfrom(fd, response, sizeof(response) - 1U, 0,
                        reinterpret_cast<sockaddr*>(&peer), &peer_size);
  }
  close(fd);
  if (received <= 0 || peer.sin_port != htons(17807)) {
    return false;
  }

  unsigned port = 0;
  char spki[45]{};
  char extra = '\0';
  response[received] = '\0';
  if (std::sscanf(response, "BOOMPI_SERVER_V2 %u %44s%c", &port, spki, &extra) != 2 ||
      port == 0U || port > 65535U) {
    return false;
  }
  char host[INET_ADDRSTRLEN]{};
  if (inet_ntop(AF_INET, &peer.sin_addr, host, sizeof(host)) == nullptr) {
    return false;
  }
  output->host = host;
  output->port = static_cast<std::uint16_t>(port);
  output->spki = spki;
  return IsEndpoint(*output);
}

bool SaveServer(const LinkConfig& server) {
  if (!IsEndpoint(server) || (mkdir(kConfigDir, 0700) != 0 && errno != EEXIST)) {
    return false;
  }
  return AtomicWrite(kServerConfig, server.host + " " + std::to_string(server.port) + " " +
                                        server.spki + "\n");
}

std::string EscapeWifiField(const std::string& input) {
  std::string escaped;
  for (const char byte : input) {
    if (byte == '\\' || byte == '"') {
      escaped += '\\';
    }
    escaped += byte;
  }
  return escaped;
}

}  // namespace

bool SaveWifi(const std::string& ssid, const std::string& password) {
  // 拒绝控制字符并转义引号，防止输入改变 wpa_supplicant 文件结构。
  if (!IsText(ssid, 1U, 32U) || !IsText(password, 8U, 63U)) {
    return false;
  }
  return AtomicWrite(
      kWifiConfig,
      "# boomPI-managed\nctrl_interface=/var/run/wpa_supplicant\nnetwork={\n  ssid=\"" +
          EscapeWifiField(ssid) + "\"\n  psk=\"" + EscapeWifiField(password) + "\"\n}\n");
}

bool detail::FindServer(const LinkConfig& configured, LinkConfig* output,
                        const std::atomic<bool>* stop) {
  if (output == nullptr) {
    return false;
  }
  *output = {};
  const char* selected = SelectInterface(stop);
  if (selected == nullptr || StopRequested(stop)) {
    return false;
  }

  // 显式端点只跳过发现；板端网卡仍要先取得地址和路由。
  if (!configured.host.empty()) {
    if (!IsEndpoint(configured)) {
      return false;
    }
    *output = configured;
    return true;
  }

  LinkConfig saved{};
  LinkConfig found{};
  const bool have_saved = LoadServer(&saved);
  // 地址可以随 DHCP 改变，已保存的公钥不能被陌生广播替换。
  // 首次发现沿用可信课堂局域网的信任边界，之后持久化并固定这个 SPKI。
  if (Discover(selected, &found) && (!have_saved || found.spki == saved.spki) &&
      SaveServer(found)) {
    *output = found;
    return true;
  }
  if (!have_saved) {
    return false;
  }
  *output = saved;
  return true;
}

}  // namespace boompi::network
