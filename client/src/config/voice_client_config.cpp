/// @file 只加载自动身份和教师固定端点；错误只显示字段名。
#include "boompi/config/voice_client_config.h"
#include <cstdlib>
#include <string_view>

namespace boompi::config {
namespace {
bool Bad(const char* name, std::string* error) {
  if (error != nullptr) *error = std::string(name) + " is invalid";
  return false;
}
bool Read(const char* name, std::size_t limit, std::string* out, std::string* error) {
  const char* value = std::getenv(name);
  if (value == nullptr) return true;
  std::size_t size = 0;
  while (size <= limit && value[size] != '\0') ++size;
  if (size > limit) return Bad(name, error);
  out->assign(value, size);
  return true;
}
bool Decimal(std::string_view value, unsigned maximum, unsigned* out) {
  if (value.empty()) return false;
  unsigned number = 0;
  for (const char ch : value) {
    if (ch < '0' || ch > '9') return false;
    const unsigned digit = static_cast<unsigned>(ch - '0');
    if (digit > maximum || number > (maximum - digit) / 10U) return false;
    number = number * 10U + digit;
  }
  *out = number;
  return true;
}
bool Ipv4(std::string_view text) {
  for (unsigned part = 0; part < 4; ++part) {
    const auto dot = text.find('.');
    const auto value = text.substr(0, dot);
    unsigned octet = 0;
    if (!Decimal(value, 255, &octet) ||
        (value.size() > 1 && value.front() == '0')) return false;
    if (part == 3) return dot == std::string_view::npos;
    if (dot == std::string_view::npos) return false;
    text.remove_prefix(dot + 1);
  }
  return false;
}
int Base64(char ch) {
  constexpr std::string_view alphabet =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  const auto index = alphabet.find(ch);
  return index == alphabet.npos ? -1 : static_cast<int>(index);
}
}  // namespace

bool IsValidDeviceId(const std::string& value) noexcept {
  if (value.size() != 36) return false;
  bool nonzero = false;
  for (std::size_t i = 0; i < value.size(); ++i) {
    if (i == 8 || i == 13 || i == 18 || i == 23) {
      if (value[i] != '-') return false;
    } else {
      const char ch = value[i];
      if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'))) return false;
      nonzero |= ch != '0';
    }
  }
  return nonzero;
}
bool IsValidSpkiSha256(const std::string& value) noexcept {
  if (value.size() != 44 || value.back() != '=') return false;
  for (std::size_t i = 0; i < 43; ++i) if (Base64(value[i]) < 0) return false;
  return (Base64(value[42]) & 3) == 0;
}
bool LoadVoiceClientConfigFromEnvironment(VoiceClientConfig* out, std::string* error) {
  if (out == nullptr) return Bad("configuration output", error);
  *out = {};
  if (!Read("BOOMPI_DEVICE_ID", 36, &out->device_id, error) ||
      !Read("BOOMPI_SERVER_IP", 15, &out->server_ip, error) ||
      !Read("BOOMPI_SERVER_SPKI_SHA256", 44, &out->server_spki_sha256, error)) return false;
  if (!IsValidDeviceId(out->device_id)) return Bad("BOOMPI_DEVICE_ID", error);
  if (!out->server_ip.empty() && !Ipv4(out->server_ip)) return Bad("BOOMPI_SERVER_IP", error);
  if (out->server_ip.empty() != out->server_spki_sha256.empty() ||
      (!out->server_spki_sha256.empty() && !IsValidSpkiSha256(out->server_spki_sha256)))
    return Bad("BOOMPI_SERVER_SPKI_SHA256", error);
  std::string port;
  unsigned number = out->server_port;
  if (!Read("BOOMPI_SERVER_PORT", 5, &port, error)) return false;
  if (!port.empty() && (!Decimal(port, 65535, &number) || number == 0))
    return Bad("BOOMPI_SERVER_PORT", error);
  out->server_port = static_cast<std::uint16_t>(number);
  if (error != nullptr) error->clear();
  return true;
}
}  // namespace boompi::config
