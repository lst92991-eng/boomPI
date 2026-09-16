#include "network.h"
#include <arpa/inet.h>
#include <fcntl.h>
#include <net/if.h>
#include <poll.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>

namespace boompi::network {
namespace {
constexpr const char* kInterfaces[] = {"eth0", "wlan0"};
constexpr char kWifiConfig[] = "/etc/wpa_supplicant.conf";
constexpr char kServerConfig[] = "/userdata/boompi/config/server.conf";

bool link_up(const char* interface) {
  std::ifstream file(std::string("/sys/class/net/") + interface + "/carrier");
  int carrier = 0;
  return (file >> carrier) && carrier == 1;
}
bool has_address(const char* interface) {
  const int fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) {
    return false;
  }
  ifreq request{};
  std::snprintf(request.ifr_name, sizeof(request.ifr_name), "%s", interface);
  const bool ok = ioctl(fd, SIOCGIFADDR, &request) == 0;
  ::close(fd);
  return ok;
}
bool tool(bool wifi, const char* interface, const std::atomic<bool>& stop) {
  if (stop.load()) {
    return false;
  }
  const auto child = fork();
  if (child == 0) {
    const int quiet = ::open("/dev/null", O_WRONLY | O_CLOEXEC);
    if (quiet >= 0) {
      dup2(quiet, STDOUT_FILENO);
      dup2(quiet, STDERR_FILENO);
    }
    if (wifi) {
      execl("/usr/bin/wpa_supplicant", "wpa_supplicant", "-B", "-i", interface, "-c",
            kWifiConfig, static_cast<char*>(nullptr));
    } else {
      execl("/sbin/udhcpc", "udhcpc", "-n", "-q", "-t", "3", "-T", "2", "-i",
            interface, static_cast<char*>(nullptr));
    }
    _exit(127);
  }
  if (child < 0) {
    return false;
  }
  int status = 0;
  for (unsigned elapsed = 0; elapsed < 12000 && !stop.load(); elapsed += 50) {
    const auto result = waitpid(child, &status, WNOHANG);
    if (result == child) {
      return WIFEXITED(status) && WEXITSTATUS(status) == 0;
    }
    if (result < 0 && errno != EINTR) {
      return false;
    }
    usleep(50000);
  }
  kill(child, SIGKILL);
  while (waitpid(child, &status, 0) < 0 && errno == EINTR) {
  }
  return false;
}
bool connect_interface(bool wifi, const std::atomic<bool>& stop) {
  const char* name = kInterfaces[wifi];
  if (stop.load()) {
    return false;
  }
  if (link_up(name) && has_address(name)) {
    return true;
  }
  if (!wifi && !link_up(name)) {
    return false;
  }
  if (wifi) {
    if (access("/sys/class/net/wlan0", F_OK) != 0 || access(kWifiConfig, R_OK) != 0) {
      return false;
    }
    // 复用系统已有supplicant；明文配置直接由维护者编辑，不再经过AP门户。
    if (access("/var/run/wpa_supplicant/wlan0", F_OK) != 0 && !tool(true, name, stop)) {
      return false;
    }
  }
  return tool(false, name, stop) && link_up(name) && has_address(name);
}
bool endpoint_valid(const config::VoiceClientConfig& endpoint) {
  in_addr address{};
  return endpoint.server_port && config::IsValidSpkiSha256(endpoint.server_spki_sha256) &&
         inet_pton(AF_INET, endpoint.server_ip.c_str(), &address) == 1;
}
bool load_server(config::VoiceClientConfig& output) {
  std::ifstream file(kServerConfig);
  unsigned port;
  std::string extra;
  if (!(file >> output.server_ip >> port >> output.server_spki_sha256) ||
      (file >> extra) || port > 65535) {
    return false;
  }
  output.server_port = static_cast<std::uint16_t>(port);
  return endpoint_valid(output);
}
bool save_server(const config::VoiceClientConfig& value) {
  if (mkdir("/userdata/boompi/config", 0700) != 0 && errno != EEXIST) {
    return false;
  }
  const std::string temporary = std::string(kServerConfig) + ".tmp";
  const int fd = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
  std::FILE* file = fd < 0 ? nullptr : fdopen(fd, "w");
  bool ok = file && fchmod(fd, 0600) == 0 &&
            std::fprintf(file, "%s %u %s\n", value.server_ip.c_str(), value.server_port,
                         value.server_spki_sha256.c_str()) > 0 &&
            std::fflush(file) == 0 && fsync(fd) == 0;
  if (file) {
    ok = std::fclose(file) == 0 && ok;
  } else if (fd >= 0) {
    ::close(fd);
  }
  if (ok && std::rename(temporary.c_str(), kServerConfig) == 0) {
    return true;
  }
  unlink(temporary.c_str());
  return false;
}
bool discover(const char* interface, config::VoiceClientConfig& output,
              const std::atomic<bool>& stop) {
  const int fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) {
    return false;
  }
  const int enabled = 1;
  sockaddr_in target{};
  target.sin_family = AF_INET;
  target.sin_port = htons(17807);
  target.sin_addr.s_addr = INADDR_BROADCAST;
  constexpr char request[] = "BOOMPI_DISCOVER_V2";
  bool ok = bind_socket(fd, interface) &&
            setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &enabled, sizeof(enabled)) == 0 &&
            sendto(fd, request, sizeof(request) - 1, 0,
                   reinterpret_cast<sockaddr*>(&target), sizeof(target)) >= 0;
  pollfd readable{fd, POLLIN, 0};
  int received = -1;
  char response[96]{};
  sockaddr_in peer{};
  socklen_t size = sizeof(peer);
  for (unsigned elapsed = 0; ok && elapsed < 800 && !stop.load(); elapsed += 50) {
    const int count = poll(&readable, 1, 50);
    if (count > 0 && (readable.revents & POLLIN)) {
      received = static_cast<int>(recvfrom(fd, response, sizeof(response) - 1, MSG_DONTWAIT,
                                           reinterpret_cast<sockaddr*>(&peer), &size));
      break;
    }
    ok = count >= 0 || errno == EINTR;
  }
  ::close(fd);
  if (received <= 0 || peer.sin_port != htons(17807) || stop.load()) {
    return false;
  }
  unsigned port;
  char pin[45], extra;
  char host[INET_ADDRSTRLEN];
  if (std::sscanf(response, "BOOMPI_SERVER_V2 %u %44s%c", &port, pin, &extra) != 2 ||
      port == 0 || port > 65535 || !inet_ntop(AF_INET, &peer.sin_addr, host, sizeof(host))) {
    return false;
  }
  output.server_ip = host;
  output.server_port = static_cast<std::uint16_t>(port);
  output.server_spki_sha256 = pin;
  return endpoint_valid(output);
}
}  // namespace
bool bind_socket(std::intptr_t descriptor, const char* interface) noexcept {
  return !interface || setsockopt(static_cast<int>(descriptor), SOL_SOCKET, SO_BINDTODEVICE,
                                  interface, std::strlen(interface) + 1) == 0;
}
bool find_server(const config::VoiceClientConfig& configured, Endpoint& output,
                 const std::atomic<bool>& stop, bool wifi_first) {
  config::VoiceClientConfig saved;
  const bool cached = load_server(saved);
  // 默认有线→无线；上次有线WSS未能READY时，下次先试无线，避免有IP却到不了服务器的死循环。
  for (const bool wifi : {wifi_first, !wifi_first}) {
    if (!connect_interface(wifi, stop) || stop.load()) {
      continue;
    }
    config::VoiceClientConfig server = configured;
    if (server.server_ip.empty()) {
      const bool found = discover(kInterfaces[wifi], server, stop) &&
                         (!cached || saved.server_spki_sha256 == server.server_spki_sha256);
      if (found && save_server(server)) {
        // 地址可变，首次课堂配对之后公钥不允许被陌生广播替换。
      } else if (cached) {
        server = saved;
      } else {
        continue;
      }
    }
    if (!stop.load() && endpoint_valid(server)) {
      output = {server, kInterfaces[wifi]};
      return true;
    }
  }
  return false;
}
}  // namespace boompi::network
