/**
 * @file network_setup.cpp
 * @brief 教学板的网卡建链、UDP 服务发现和配置保存。
 *
 * FindServer 由 voice_net 线程调用；配网页只调用 save_wifi 保存配置。
 * DHCP、外部命令和文件写入都可能等待，不能放到采集或播放线程。
 *
 * 地址链路先 SelectInterface，显式端点可直接返回；自动模式继续 Discover →
 * 比对已缓存 SPKI → SaveServer，发现失败时可沿用 LoadServer 缓存。
 * 这里只返回连接候选，持有相应公钥的证明由
 * voice_net.cpp 中的 TLS 握手完成，网卡可用也不等于服务器已经 ready。
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

namespace boompi::voice_net {
namespace {

// 网卡名来自教学板系统；发现的服务器候选及 pin 保存在 userdata，更新应用时不覆盖。
constexpr char kEthernet[] = "eth0";
constexpr char kWifi[] = "wlan0";
constexpr char kWifiConfig[] = "/etc/wpa_supplicant.conf";
constexpr char kConfigDir[] = "/userdata/boompi/config";
constexpr char kServerConfig[] = "/userdata/boompi/config/server.conf";

/// @brief 网络准备的协作退出点；没有传退出标志的调用不具备外部取消请求。
bool StopRequested(const std::atomic<bool>* stop) {
  return stop != nullptr && stop->load(std::memory_order_acquire);
}

/**
 * @brief 按字节限制 Wi-Fi 字段长度并拒绝控制字符，防止生成多行配置。
 *
 * 中文 SSID 也按编码字节计数，不按显示字符数计数；引号和反斜杠交给写入前转义。
 */
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

/// @brief 只做端点格式检查；合法 IPv4、端口和 Base64 pin 不代表远端可信或当前可达。
bool IsEndpoint(const config::VoiceClientConfig& server) {
  in_addr address{};
  return server.server_port != 0U && config::IsValidSpkiSha256(server.server_spki_sha256) &&
         inet_pton(AF_INET, server.server_ip.c_str(), &address) == 1;
}

/**
 * @brief 完整写好临时文件后再替换配置，写入失败时保留旧文件。
 *
 * Wi-Fi 密码只允许文件属主读取；O_NOFOLLOW 防止临时路径被符号链接替换。
 * 先 fsync 再 rename，避免读者看到只写了一半的配置。
 * @return 写入、同步、关闭和替换全部成功时为 true；失败清理临时文件并返回 false。
 */
bool AtomicWrite(const char* path, const std::string& text) {
  const std::string temporary = std::string(path) + ".tmp";
  const int fd =
      ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
  if (fd < 0) {
    return false;
  }
  if (fchmod(fd, 0600) != 0) {
    ::close(fd);
    unlink(temporary.c_str());
    return false;
  }

  bool succeeded = true;
  std::size_t offset = 0;
  // write 允许短写，EINTR 也不说明数据已完整写入；推进实际字节数才能安全替换旧配置。
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
  if (::close(fd) != 0) {
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

/// @brief 读取板端 sysfs 的 carrier/operstate；读取失败视为条件不成立，交给后续回退。
bool NetValueIs(const char* interface, const char* item, const char* expected) {
  std::ifstream input(std::string("/sys/class/net/") + interface + "/" + item);
  std::string value;
  return (input >> value) && value == expected;
}

/// @brief 用 ioctl 判断指定网卡是否已有 IPv4 地址，避免每次重连都重复运行 DHCP。
bool HasIpv4(const char* interface) {
  const int fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) {
    return false;
  }
  ifreq request{};
  std::snprintf(request.ifr_name, sizeof(request.ifr_name), "%s", interface);
  const bool found = ioctl(fd, SIOCGIFADDR, &request) == 0;
  ::close(fd);
  return found;
}

/**
 * @brief 启动 supplicant 或 DHCP，并在退出请求或超时后回收子进程。
 *
 * supplicant 的 -B 留下系统配网服务，DHCP 最多尝试三次。父进程每 100 ms
 * 检查 stop，最长等 12 秒；工具输出不进入日志，以免泄露凭据。
 * start_wifi 为 true 时启动 Wi-Fi 关联服务，否则为指定网卡获取 DHCP 地址。
 * 使用 execl 的独立参数，不把 SSID、密码或配置内容拼进 shell 命令。
 */
bool RunNetworkTool(bool start_wifi, const char* interface, const std::atomic<bool>* stop) {
  const pid_t child = fork();
  if (child == 0) {
    const int null_fd = ::open("/dev/null", O_WRONLY | O_CLOEXEC);
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
    // exec 失败时只结束 fork 出的子进程，不能返回后继续运行一份客户端网络逻辑。
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

/**
 * @brief 优先复用有线地址；只有有线不可用时才进入 Wi-Fi 配网流程。
 *
 * eth0 有载波且有 IPv4 即可使用，否则尝试有线 DHCP。备用 wlan0 需要已有配置文件，
 * 可复用其现有 IPv4，或先启动 supplicant 再取地址；所有失败最终返回 nullptr。
 * 返回值指向静态网卡名，调用方不拥有其内存，也不在此函数里等待服务器握手。
 */
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

/**
 * @brief 读取缓存的 IPv4、端口、SPKI 三字段，拒绝缺项、额外字段和越界端口。
 *
 * 只由 FindServer 传入有效指针；失败时不能使用局部解析结果，须继续发现或报错。
 */
bool LoadServer(config::VoiceClientConfig* output) {
  std::ifstream input(kServerConfig);
  unsigned port = 0;
  std::string extra;
  if (!(input >> output->server_ip >> port >> output->server_spki_sha256) || (input >> extra) ||
      port > 65535U) {
    return false;
  }
  output->server_port = static_cast<std::uint16_t>(port);
  return IsEndpoint(*output);
}

/**
 * @brief 在选定网卡上广播发现请求，用响应源地址作为服务器地址。
 *
 * SO_BINDTODEVICE 避免请求从另一张网卡发出。UDP 只能提供地址和公钥提示，
 * 已配对设备还要与缓存 SPKI 比对，随后由 TLS 验证持有该公钥的服务器。
 * 一次调用仅发送一次广播、读取一次响应，最多等 800 ms；重试节奏由 voice_net 管理。
 * 响应必须来自 UDP 17807，WSS 端口和 44 字符 pin 来自严格格式的文本负载。
 */
bool Discover(const char* interface, config::VoiceClientConfig* output) {
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
  ::close(fd);
  if (received <= 0 || peer.sin_port != htons(17807)) {
    return false;
  }

  unsigned port = 0;
  char spki[45]{};
  char extra = '\0';
  // 尾部 %c 用来发现多余字节；仅恰好匹配端口和 pin 两项时才接受该响应。
  response[received] = '\0';
  if (std::sscanf(response, "BOOMPI_SERVER_V2 %u %44s%c", &port, spki, &extra) != 2 ||
      port == 0U || port > 65535U) {
    return false;
  }
  char host[INET_ADDRSTRLEN]{};
  if (inet_ntop(AF_INET, &peer.sin_addr, host, sizeof(host)) == nullptr) {
    return false;
  }
  output->server_ip = host;
  output->server_port = static_cast<std::uint16_t>(port);
  output->server_spki_sha256 = spki;
  return IsEndpoint(*output);
}

/**
 * @brief 将合法发现结果保存为后续地址回退与 SPKI 比对基准。
 *
 * 首次可信课堂发现会在 TLS 建链前保存候选 pin；保存失败不把未持久化候选作为成功。
 * 后续发现可更新 DHCP 地址，但 FindServer 不允许陌生 pin 覆盖已保存身份。
 */
bool SaveServer(const config::VoiceClientConfig& server) {
  if (!IsEndpoint(server) || (mkdir(kConfigDir, 0700) != 0 && errno != EEXIST)) {
    return false;
  }
  return AtomicWrite(kServerConfig, server.server_ip + " " +
                                        std::to_string(server.server_port) + " " +
                                        server.server_spki_sha256 + "\n");
}

/// @brief 对已通过长度/控制字符检查的字段转义，确保引号内输入不改变 supplicant 语法。
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

/**
 * @brief UI worker 的保存入口：校验输入 → 转义字段 → 完整替换 wpa_supplicant 配置。
 *
 * 此处不做网络连接，也不打印字段值；下次网络准备阶段使用保存后的配置尝试 Wi-Fi。
 */
bool save_wifi(const std::string& ssid, const std::string& password) {
  // 拒绝控制字符并转义引号，防止输入改变 wpa_supplicant 文件结构。
  if (!IsText(ssid, 1U, 32U) || !IsText(password, 8U, 63U)) {
    return false;
  }
  return AtomicWrite(
      kWifiConfig,
      "# boomPI-managed\nctrl_interface=/var/run/wpa_supplicant\nnetwork={\n  ssid=\"" +
          EscapeWifiField(ssid) + "\"\n  psk=\"" + EscapeWifiField(password) + "\"\n}\n");
}

/**
 * @brief 将网卡选择与服务器选择串成网络线程的一次准备动作。
 *
 * 自动模式先探测当前广播地址，若 pin 与旧缓存相同则更新地址；任何发现、匹配或保存
 * 步骤失败都尝试使用旧缓存。返回缓存不承诺地址仍有效，连接失败由外层退避重试。
 */
bool detail::FindServer(const config::VoiceClientConfig& configured,
                        config::VoiceClientConfig* output, const std::atomic<bool>* stop) {
  if (output == nullptr) {
    return false;
  }
  *output = {};
  const char* selected = SelectInterface(stop);
  if (selected == nullptr || StopRequested(stop)) {
    return false;
  }

  // 显式端点只跳过发现；板端网卡仍要先取得地址和路由。
  if (!configured.server_ip.empty()) {
    if (!IsEndpoint(configured)) {
      return false;
    }
    *output = configured;
    return true;
  }

  config::VoiceClientConfig saved{};
  config::VoiceClientConfig found{};
  const bool have_saved = LoadServer(&saved);
  // 地址可以随 DHCP 改变，已保存的公钥不能被陌生广播替换。
  // 首次发现沿用可信课堂局域网的信任边界，之后持久化并固定这个 SPKI。
  if (Discover(selected, &found) &&
      (!have_saved || found.server_spki_sha256 == saved.server_spki_sha256) &&
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

}  // namespace boompi::voice_net
