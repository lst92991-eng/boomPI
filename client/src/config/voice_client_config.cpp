#include "boompi/config/voice_client_config.h"

#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <string_view>

namespace boompi::config {
namespace {
std::string_view environment(const char* name) {
  const char* text = std::getenv(name);
  return text ? text : "";
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
  // 先借用环境字符串并校验，全部成功后再复制一次；不把字段值写入日志。
  const auto id = environment("BOOMPI_DEVICE_ID");
  const auto ip = environment("BOOMPI_SERVER_IP");
  const auto pin = environment("BOOMPI_SERVER_SPKI_SHA256");
  const auto port = environment("BOOMPI_SERVER_PORT");
  unsigned number = output->server_port;
  if (!IsValidDeviceId(id)) {
    return fail("BOOMPI_DEVICE_ID");
  }
  if (!ip.empty() && (ip.size() > 15 || !ipv4(ip))) {
    return fail("BOOMPI_SERVER_IP");
  }
  if (ip.empty() != pin.empty() || (!pin.empty() && !IsValidSpkiSha256(pin))) {
    return fail("BOOMPI_SERVER_SPKI_SHA256");
  }
  if (!port.empty() && (port.size() > 5 || !decimal(port, 65535, number) || number == 0)) {
    return fail("BOOMPI_SERVER_PORT");
  }
  *output = {std::string(id), std::string(ip), static_cast<std::uint16_t>(number),
             std::string(pin)};
  if (error) {
    error->clear();
  }
  return true;
}
}  // namespace boompi::config
