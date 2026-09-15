/**
 * @file voice_client_config.cpp
 *  @brief 读取设备身份和可选固定端点，在启动线程前检查格式。
 *
 * 读取顺序：限长复制 → 设备身份 → 地址与 SPKI 配对 → 端口。成功后 main 才能启动
 * application；本文件不读声学参数，也不把环境变量原值写入错误消息。
 */
#include "boompi/config/voice_client_config.h"

#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <cstring>
#include <string_view>

namespace boompi::config {
namespace {

/** @brief 用字段名说明配置失败，避免把配置内容带入普通日志。 */
bool ConfigError(const char* name, std::string* error) {
  if (error != nullptr) {
    *error = std::string(name) + " is invalid";
  }
  return false;
}

/**
 * @brief 检查环境变量长度后再复制；getenv 提供以零结尾的进程环境字符串。
 * 未设置时保留 output 原值；显式空串会覆盖原值。调用者再按字段决定空值是否合法。
 */
bool ReadEnvironment(const char* name, std::size_t limit, std::string* output,
                     std::string* error) {
  const char* value = std::getenv(name);
  if (value == nullptr) {
    return true;
  }

  // 先限长再复制，外部环境变量不能让后续URI/JSON处理无界增长。
  const std::size_t size = std::strlen(value);
  if (size > limit) {
    return ConfigError(name, error);
  }
  output->assign(value, size);
  return true;
}

/** @brief 解析不带符号的十进制整数，范围为 0..maximum；仅成功时写入 output。 */
bool ParseDecimal(std::string_view text, unsigned maximum, unsigned* output) {
  if (text.empty()) {
    return false;
  }
  unsigned number = 0;
  const char* end = text.data() + text.size();
  const auto parsed = std::from_chars(text.data(), end, number);
  if (parsed.ec != std::errc{} || parsed.ptr != end || number > maximum) {
    return false;
  }
  *output = number;
  return true;
}

/** @brief 用四个 0..255 的十进制段检查固定端点，不接受主机名、缩写或前导零。 */
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

/** @brief 返回标准 Base64 字符的六位数值；非法字符返回 -1，填充符由外层单独检查。 */
int Base64Digit(char ch) {
  constexpr std::string_view alphabet =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  const std::size_t index = alphabet.find(ch);
  return index == std::string_view::npos ? -1 : static_cast<int>(index);
}
}  // namespace

bool IsValidDeviceId(const std::string& value) noexcept {
  // 这里验证协议可接受的文本形式；身份生成由安装脚本负责，不在客户端自行重建。
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
  // 32 字节摘要编码为 43 个数据字符加一个 '='；不在此解码或读取远端证书。
  if (value.size() != 44 || value.back() != '=') {
    return false;
  }
  if (!std::all_of(value.begin(), value.end() - 1, [](char ch) {
        return Base64Digit(ch) >= 0;
      })) {
    return false;
  }
  // SHA-256为32字节，末个Base64数据字符有两个填充位，必须为0。
  return (Base64Digit(value[42]) & 3) == 0;
}

bool LoadClientConfig(VoiceClientConfig* output, std::string* error) {
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
  // 固定地址和固定公钥必须成对，防止只指定地址却误用另一台课堂服务器的身份。
  const bool has_fingerprint = !output->server_spki_sha256.empty();
  if (has_address && !IsIpv4(output->server_ip)) {
    return ConfigError("BOOMPI_SERVER_IP", error);
  }
  if (has_address != has_fingerprint ||
      (has_fingerprint && !IsValidSpkiSha256(output->server_spki_sha256))) {
    return ConfigError("BOOMPI_SERVER_SPKI_SHA256", error);
  }

  std::string port;
  // 空端口使用结构体默认值；合法非空端口范围为 1..65535，转换到 uint16_t 前校验。
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
