#include "boompi/config/voice_client_config.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <fstream>
#include <string_view>
#include <utility>

namespace boompi::config {
namespace {
constexpr char kConfigPath[] = "/userdata/boompi/config/client.conf";

bool create_config() {
  for (const char* directory : {"/userdata/boompi", "/userdata/boompi/config"}) {
    if (mkdir(directory, 0700) < 0 && errno != EEXIST) {
      return false;
    }
  }
  std::ifstream random_id("/proc/sys/kernel/random/uuid");
  std::string id;
  if (!(random_id >> id) || !IsValidDeviceId(id)) {
    return false;
  }
  // 只创建缺失配置，不覆盖已有身份；持久化成功后才允许连接服务端。
  const int fd = open(kConfigPath, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
  if (fd < 0) {
    return errno == EEXIST;
  }
  const std::string text = "device_id=" + id + "\n";
  bool ok = write(fd, text.data(), text.size()) == static_cast<ssize_t>(text.size()) &&
            fsync(fd) == 0;
  if (close(fd) < 0) {
    ok = false;
  }
  if (!ok) {
    unlink(kConfigPath);
  }
  return ok;
}
bool decimal(std::string_view text, unsigned limit, unsigned& number) {
  const auto end = text.data() + text.size();
  const auto parsed = std::from_chars(text.data(), end, number);
  return !text.empty() && parsed.ec == std::errc{} && parsed.ptr == end && number <= limit;
}
bool ipv4(std::string_view text) {
  for (unsigned part = 0; part < 4; ++part) {
    const auto dot = text.find('.');
    const auto word = text.substr(0, dot);
    unsigned octet = 0;
    if (!decimal(word, 255, octet) || (word.size() > 1 && word.front() == '0') ||
        ((part == 3) != (dot == std::string_view::npos))) {
      return false;
    }
    if (part != 3) {
      text.remove_prefix(dot + 1);
    }
  }
  return true;
}
int base64(char c) {
  constexpr std::string_view alphabet =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  const auto at = alphabet.find(c);
  return at == std::string_view::npos ? -1 : static_cast<int>(at);
}
}  // namespace
bool IsValidDeviceId(std::string_view text) noexcept {
  if (text.size() != 36) {
    return false;
  }
  bool nonzero = false;
  for (std::size_t i = 0; i < text.size(); ++i) {
    const bool dash = i == 8 || i == 13 || i == 18 || i == 23;
    const char c = text[i];
    if (dash ? c != '-' : !((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
      return false;
    }
    nonzero = nonzero || (!dash && c != '0');
  }
  return nonzero;
}
bool IsValidSpkiSha256(std::string_view text) noexcept {
  // 32字节的标准Base64，末数据字符低两位必须为0；TLS仍另验服务器持有此公钥。
  return text.size() == 44 && text.back() == '=' &&
         std::all_of(text.begin(), text.end() - 1,
                     [](char c) {
                       return base64(c) >= 0;
                     }) &&
         (base64(text[42]) & 3) == 0;
}
bool LoadClientConfig(VoiceClientConfig* output, std::string* error) {
  const auto fail = [error](const char* field) {
    if (error) {
      *error = std::string(field) + " is invalid";
    }
    return false;
  };
  if (!output) {
    return fail("configuration output");
  }
  *output = {};
  std::ifstream file(kConfigPath);
  if (!file) {
    if (errno != ENOENT || !create_config()) {
      return fail("client.conf access");
    }
    file.clear();
    file.open(kConfigPath);
  }
  file.seekg(0, std::ios::end);
  const std::streamoff size = file.tellg();
  if (size <= 0 || size > 4096) {
    return fail("client.conf size");
  }
  file.seekg(0);
  std::string id, ip, pin, port, line;
  while (std::getline(file, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    if (line.empty() || line.front() == '#') {
      continue;
    }
    const auto equal = line.find('=');
    if (equal == std::string::npos) {
      return fail("client.conf format");
    }
    const auto name = std::string_view(line).substr(0, equal);
    const auto value = std::string_view(line).substr(equal + 1);
    if (name == "device_id") {
      id = value;
    } else if (name == "server_ip") {
      ip = value;
    } else if (name == "server_port") {
      port = value;
    } else if (name == "server_spki_sha256") {
      pin = value;
    } else {
      return fail("client.conf field");
    }
  }
  if (file.bad()) {
    return fail("client.conf read");
  }
  unsigned number = output->server_port;
  if (!IsValidDeviceId(id)) {
    return fail("device_id");
  }
  if (!ip.empty() && (ip.size() > 15 || !ipv4(ip))) {
    return fail("server_ip");
  }
  if (ip.empty() != pin.empty() || (!pin.empty() && !IsValidSpkiSha256(pin))) {
    return fail("server_spki_sha256");
  }
  if (!port.empty() && (port.size() > 5 || !decimal(port, 65535, number) || number == 0)) {
    return fail("server_port");
  }
  *output = {std::move(id), std::move(ip), static_cast<std::uint16_t>(number), std::move(pin)};
  if (error) {
    error->clear();
  }
  return true;
}
}  // namespace boompi::config
