#include "network.h"
namespace boompi::network {
bool find_server(const config::VoiceClientConfig& configured, Endpoint& output,
                 const std::atomic<bool>& stop, bool) {
  output = {configured, nullptr};
  return !stop.load() && !configured.server_ip.empty() && configured.server_port &&
         config::IsValidSpkiSha256(configured.server_spki_sha256);
}
bool bind_socket(std::intptr_t, const char*) noexcept {
  return true;
}
}  // namespace boompi::network
