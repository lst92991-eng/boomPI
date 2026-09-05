/** @file voice_client_config.cpp
 *  @brief 读取设备身份和可选固定端点，在启动线程前检查格式。
 */
#include "boompi/config/voice_client_config.h"

#include <cstdlib>
#include <string_view>

namespace boompi::config {
namespace {

bool ConfigError(const char* name, std::string* error) {
  if (error != nullptr) {
    *error = std::string(name) + " is invalid";
  }
  return false;
}

bool ReadEnvironment(const char* name, std::size_t limit, std::string* output,
                     std::string* error) {
  const char* value = std::getenv(name);
  if (value == nullptr) {
    return true;
  }

  // 先限长再复制，外部环境变量不能让后续URI/JSON处理无界增长。
  std::size_t size = 0;
  while (size <= limit && value[size] != '\0') {
    ++size;
  }
  if (size > limit) {
    return ConfigError(name, error);
  }
  output->assign(value, size);
  return true;
}

bool ParseDecimal(std::string_view text, unsigned maximum, unsigned* output) {
  if (text.empty()) {
    return false;
  }
  unsigned number = 0;
  for (char ch : text) {
    if (ch < '0' || ch > '9') {
      return false;
    }
    const unsigned digit = static_cast<unsigned>(ch - '0');
    // 在乘10之前检查上限，避免溢出后反而落回合法范围。
    if (digit > maximum || number > (maximum - digit) / 10U) {
      return false;
    }
    number = number * 10U + digit;
  }
  *output = number;
  return true;
}

bool IsIpv4(std::string_view text) {
  for (unsigned part = 0; part < 4; ++part) {
    const std::size_t dot = text.find('.');
    const std::string_view value = text.substr(0, dot);
    unsigned octet = 0;
    if (!ParseDecimal(value, 255, &octet)) {
      return false;
    }
    // 只接受标准十进制表示，避免不同库将前导0解释成八进制。
    if (value.size() > 1 && value.front() == '0') {
      return false;
    }
    if (part == 3) {
      return dot == std::string_view::npos;
    }
    if (dot == std::string_view::npos) {
      return false;
    }
    text.remove_prefix(dot + 1);
  }
  return false;
}

int Base64Digit(char ch) {
  constexpr std::string_view alphabet =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  const std::size_t index = alphabet.find(ch);
  if (index == std::string_view::npos) {
    return -1;
  }
  return static_cast<int>(index);
}
}  // namespace

bool IsValidDeviceId(const std::string& value) noexcept {
  if (value.size() != 36) {
    return false;
  }
  bool nonzero = false;
  for (std::size_t i = 0; i < value.size(); ++i) {
    if (i == 8 || i == 13 || i == 18 || i == 23) {
      if (value[i] != '-') {
        return false;
      }
      continue;
    }
    const char ch = value[i];
    const bool decimal = ch >= '0' && ch <= '9';
    const bool lowercase_hex = ch >= 'a' && ch <= 'f';
    if (!decimal && !lowercase_hex) {
      return false;
    }
    if (ch != '0') {
      nonzero = true;
    }
  }
  return nonzero;
}

bool IsValidSpkiSha256(const std::string& value) noexcept {
  if (value.size() != 44 || value.back() != '=') {
    return false;
  }
  for (std::size_t i = 0; i < 43; ++i) {
    if (Base64Digit(value[i]) < 0) {
      return false;
    }
  }
  // SHA-256为32字节，末个Base64数据字符有两个填充位，必须为0。
  return (Base64Digit(value[42]) & 3) == 0;
}

bool LoadVoiceClientConfigFromEnvironment(VoiceClientConfig* output, std::string* error) {
  if (output == nullptr) {
    return ConfigError("configuration output", error);
  }
  *output = {};
  if (!ReadEnvironment("BOOMPI_DEVICE_ID", 36, &output->device_id, error) ||
      !ReadEnvironment("BOOMPI_SERVER_IP", 15, &output->server_ip, error) ||
      !ReadEnvironment("BOOMPI_SERVER_SPKI_SHA256", 44, &output->server_spki_sha256, error)) {
    return false;
  }
  if (!IsValidDeviceId(output->device_id)) {
    return ConfigError("BOOMPI_DEVICE_ID", error);
  }

  const bool has_address = !output->server_ip.empty();
  const bool has_fingerprint = !output->server_spki_sha256.empty();
  if (has_address && !IsIpv4(output->server_ip)) {
    return ConfigError("BOOMPI_SERVER_IP", error);
  }
  if (has_address != has_fingerprint ||
      (has_fingerprint && !IsValidSpkiSha256(output->server_spki_sha256))) {
    return ConfigError("BOOMPI_SERVER_SPKI_SHA256", error);
  }

  std::string port;
  unsigned number = output->server_port;
  if (!ReadEnvironment("BOOMPI_SERVER_PORT", 5, &port, error)) {
    return false;
  }
  if (!port.empty() && (!ParseDecimal(port, 65535, &number) || number == 0)) {
    return ConfigError("BOOMPI_SERVER_PORT", error);
  }
  output->server_port = static_cast<std::uint16_t>(number);
  if (error != nullptr) {
    error->clear();
  }
  return true;
}
}  // namespace boompi::config
