#include "network.h"
#include <sys/socket.h>
#include <unistd.h>
#include <cstring>
#include <iostream>

int main(int argc, char** argv) {
  std::atomic<bool> stop{true};
  boompi::config::VoiceClientConfig config;
  config.server_ip = "192.0.2.1";
  config.server_spki_sha256 = "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=";
  boompi::network::Endpoint endpoint;
  if (boompi::network::find_server(config, endpoint, stop)) {
    return 1;
  }
  const int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0 || !boompi::network::bind_socket(fd, nullptr) ||
      boompi::network::bind_socket(fd, "missing-netif")) {
    return 2;
  }
  ::close(fd);
  // CI在独立network namespace中配置两张dummy网卡，再验证实际选择与socket绑定。
  if (argc > 1) {
    stop.store(false);
    const bool wifi_first = argc > 2;
    if (!boompi::network::find_server(config, endpoint, stop, wifi_first) ||
        std::strcmp(endpoint.interface, argv[1]) != 0) {
      return 3;
    }
    const int socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    char selected[32]{};
    socklen_t size = sizeof(selected);
    const bool ok = boompi::network::bind_socket(socket_fd, endpoint.interface) &&
        getsockopt(socket_fd, SOL_SOCKET, SO_BINDTODEVICE, selected, &size) == 0 &&
        std::strcmp(selected, argv[1]) == 0;
    ::close(socket_fd);
    if (!ok) {
      return 4;
    }
  }
  return 0;
}
