#include "boompi/network/network_bootstrap.h"

#include "boompi/config/voice_client_config.h"

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

namespace boompi::network { namespace {

bool StopRequested(const std::atomic<bool>* stop) {
  return stop != nullptr && stop->load(std::memory_order_acquire);
}

constexpr char kEthernet[] = "eth0", kWifi[] = "wlan0", kWifiConfig[] = "/etc/wpa_supplicant.conf";
constexpr char kConfigDir[] = "/userdata/boompi/config", kServerConfig[] = "/userdata/boompi/config/server.conf";

bool IsText(const std::string& text, std::size_t minimum, std::size_t maximum) {
  if (text.size() < minimum || text.size() > maximum) return false;
  for (const unsigned char byte : text) if (byte < 0x20U || byte == 0x7FU) return false;
  return true;
}

bool IsEndpoint(const NetworkBootstrapResult& server) {
  in_addr address{};
  return server.port != 0U &&
         config::IsValidSpkiSha256(server.spki_sha256_base64) &&
         inet_pton(AF_INET, server.host.c_str(), &address) == 1;
}

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

bool NetValueIs(const char* interface, const char* item, const char* expected) {
  std::ifstream input(std::string("/sys/class/net/") + interface + "/" + item);
  std::string value; return (input >> value) && value == expected;
}

bool HasIpv4(const char* interface) {
  const int fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) return false;
  ifreq request{}; std::snprintf(request.ifr_name, sizeof(request.ifr_name), "%s", interface);
  const bool found = ioctl(fd, SIOCGIFADDR, &request) == 0; close(fd); return found;
}

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

bool LoadServer(NetworkBootstrapResult* output) {
  std::ifstream input(kServerConfig); unsigned port = 0; std::string extra;
  if (!(input >> output->host >> port >> output->spki_sha256_base64) ||
      (input >> extra) || port > 65535U) return false;
  output->port = static_cast<std::uint16_t>(port); return IsEndpoint(*output);
}

bool Discover(const char* interface, NetworkBootstrapResult* output) {
  const int fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) return false;
  const int enabled = 1; timeval timeout{0, 800000};
  bool ok = setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &enabled, sizeof(enabled)) == 0 &&
            setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0 &&
            setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, interface,
                       std::char_traits<char>::length(interface) + 1U) == 0;
  sockaddr_in target{}; target.sin_family = AF_INET;
  target.sin_port = htons(17807); target.sin_addr.s_addr = INADDR_BROADCAST;
  constexpr char request[] = "BOOMPI_DISCOVER_V1";
  if (ok) ok = sendto(fd, request, sizeof(request) - 1U, 0,
                      reinterpret_cast<sockaddr*>(&target), sizeof(target)) >= 0;
  char response[96]{}; sockaddr_in peer{}; socklen_t peer_size = sizeof(peer);
  const ssize_t bytes = ok ? recvfrom(fd, response, sizeof(response) - 1U, 0,
                                      reinterpret_cast<sockaddr*>(&peer), &peer_size) : -1;
  close(fd);
  if (bytes <= 0 || peer.sin_port != htons(17807)) return false;
  unsigned port = 0; char spki[45]{}, extra = '\0'; response[bytes] = '\0';
  if (std::sscanf(response, "BOOMPI_SERVER_V1 %u %44s%c", &port, spki, &extra) != 2 ||
      port == 0U || port > 65535U) return false;
  char host[INET_ADDRSTRLEN]{};
  if (inet_ntop(AF_INET, &peer.sin_addr, host, sizeof(host)) == nullptr) return false;
  output->host = host; output->port = static_cast<std::uint16_t>(port);
  output->spki_sha256_base64 = spki; return IsEndpoint(*output);
}

}  // namespace

bool NetworkBootstrap::SaveWifi(const std::string& ssid, const std::string& password) {
  if (!IsText(ssid, 1U, 32U) || !IsText(password, 8U, 63U)) return false;
  auto quote = [](const std::string& input) { std::string output;
    for (const char byte : input) { if (byte == '\\' || byte == '"') { output += '\\'; } output += byte; }
    return output; };
  // 密码只写入 0600 文件，不出现在普通日志里。
  return AtomicWrite(kWifiConfig, "# boomPI-managed\nctrl_interface=/var/run/wpa_supplicant\nnetwork={\n  ssid=\"" +
                     quote(ssid) + "\"\n  psk=\"" + quote(password) + "\"\n}\n");
}

bool NetworkBootstrap::SaveServer(const NetworkBootstrapResult& server) {
  if (!IsEndpoint(server) || (mkdir(kConfigDir, 0700) != 0 && errno != EEXIST)) return false;
  return AtomicWrite(kServerConfig, server.host + " " + std::to_string(server.port) + " " + server.spki_sha256_base64 + "\n");
}

bool NetworkBootstrap::Start(const NetworkBootstrapResult* explicit_server,
                             NetworkBootstrapResult* output,
                             const std::atomic<bool>* stop) {
  if (output == nullptr) return false;
  *output = {}; const char* selected = nullptr;
  // 有线已有地址时不重复 DHCP，链路本地地址同样可用于局域网。
  if (NetValueIs(kEthernet, "carrier", "1") &&
      (HasIpv4(kEthernet) ||
       (RunNetworkTool(false, kEthernet, stop) && HasIpv4(kEthernet)))) {
    selected = kEthernet;
  } else {
    if (access("/sys/class/net/wlan0", F_OK) != 0 || access(kWifiConfig, R_OK) != 0) return false;
    if (!HasIpv4(kWifi) &&
        ((!NetValueIs(kWifi, "operstate", "up") &&
          !RunNetworkTool(true, kWifi, stop)) ||
         !RunNetworkTool(false, kWifi, stop) || !HasIpv4(kWifi))) return false;
    selected = kWifi;
  }
  if (StopRequested(stop)) return false;
  // 显式地址只替代服务器查找，不绕过上面的网卡建链和 DHCP。
  if (explicit_server != nullptr) {
    if (!IsEndpoint(*explicit_server)) return false;
    *output = *explicit_server;
    return true;
  }
  NetworkBootstrapResult saved{}, found{}; const bool have_saved = LoadServer(&saved);
  if (Discover(selected, &found) &&
      (!have_saved || found.spki_sha256_base64 == saved.spki_sha256_base64) &&
      NetworkBootstrap::SaveServer(found)) {
    *output = found; return true;
  }
  if (!have_saved) return false;
  *output = saved; return true;
}

}  // namespace boompi::network
