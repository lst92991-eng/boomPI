/// @file 板端环境配置解析与边界校验。
#include "boompi/config/voice_client_config.h"

#include <cerrno>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <string>

namespace boompi::config {
namespace {

bool Bad(const char* const variable_name, std::string* const error) {
  if (error != nullptr)
    *error = std::string(variable_name) + " is missing or outside allowed bounds";
  return false;
}

bool ReadString(const char* name, bool required, std::size_t maximum,
                std::string* output, std::string* error) {
  const char* value = std::getenv(name);
  if (value == nullptr || *value == '\0') {
    output->clear(); return !required || Bad(name, error);
  }
  std::size_t size = 0U;
  while (size <= maximum && value[size] != '\0') ++size;
  if (size > maximum) return Bad(name, error);
  output->assign(value, size); return true;
}

bool ReadUint(const char* name, std::uint32_t fallback,
              std::uint32_t minimum, std::uint32_t maximum,
              std::uint32_t* output, std::string* error) {
  const char* text = std::getenv(name);
  if (text == nullptr || *text == '\0') { *output = fallback; return true; }
  char* end = nullptr; errno = 0;
  const unsigned long parsed = std::strtoul(text, &end, 10);
  if (errno != 0 || *end != '\0' || parsed < minimum || parsed > maximum)
    return Bad(name, error);
  *output = static_cast<std::uint32_t>(parsed); return true;
}

bool ReadFloat(const char* name, float fallback, float minimum, float maximum,
               float* output, std::string* error) {
  const char* text = std::getenv(name);
  if (text == nullptr || *text == '\0') {
    *output = fallback;
    return true;
  }
  char* end = nullptr;
  errno = 0;
  const float parsed = std::strtof(text, &end);
  if (errno != 0 || *end != '\0' || !std::isfinite(parsed) ||
      parsed < minimum || parsed > maximum)
    return Bad(name, error);
  *output = parsed;
  return true;
}

bool ReadPolarity(const char* name, std::int8_t* output, std::string* error) {
  const char* value = std::getenv(name);
  if (value == nullptr || *value == '\0') { *output = 1; return true; }
  if (value[0] == '1' && value[1] == '\0') { *output = 1; return true; }
  if (value[0] == '-' && value[1] == '1' && value[2] == '\0') { *output = -1; return true; }
  return Bad(name, error);
}

bool IsSnowboySensitivity(const std::string& text) noexcept {
  char* end = nullptr; errno = 0;
  const float value = std::strtof(text.c_str(), &end);
  return errno == 0 && end == text.c_str() + text.size() &&
         value >= 0.0F && value <= 1.0F;
}

int Base64Value(const char digit) noexcept {
  if (digit >= 'A' && digit <= 'Z') return digit - 'A';
  if (digit >= 'a' && digit <= 'z') return digit - 'a' + 26;
  if (digit >= '0' && digit <= '9') return digit - '0' + 52;
  if (digit == '+') return 62;
  if (digit == '/') return 63;
  return -1;
}

}  // namespace

bool IsValidDeviceId(const std::string& value) noexcept {
  if (value.size() != 36U || value[8] != '-' || value[13] != '-' ||
      value[18] != '-' || value[23] != '-') {
    return false;
  }
  bool nonzero = false;
  for (std::size_t index = 0; index < value.size(); ++index) {
    if (index == 8U || index == 13U || index == 18U || index == 23U) continue;
    const char digit = value[index];
    if (!((digit >= '0' && digit <= '9') || (digit >= 'a' && digit <= 'f'))) {
      return false;
    }
    nonzero = nonzero || digit != '0';
  }
  return nonzero;
}

bool IsValidSpkiSha256(const std::string& value) noexcept {
  if (value.size() != 44U || value.back() != '=') return false;
  for (std::size_t index = 0; index < 43U; ++index) {
    if (Base64Value(value[index]) < 0) return false;
  }
  // SHA-256 是 32 字节；末个 base64 数据字符的两个 padding bit 必须为零。
  return (static_cast<unsigned>(Base64Value(value[42])) & 0x03U) == 0U;
}

bool LoadVoiceClientConfigFromEnvironment(VoiceClientConfig* const output,
                                          std::string* const error) {
  if (output == nullptr) {
    if (error != nullptr) *error = "voice client config output is null";
    return false;
  }
  *output = VoiceClientConfig{};
  if (!ReadString("BOOMPI_SERVER_IP", false, 64U, &output->server_ip, error) ||
      !ReadString("BOOMPI_SERVER_SPKI_SHA256", false, 64U, &output->server_spki_sha256, error) ||
      !ReadString("BOOMPI_DEVICE_ID", true, 36U, &output->device_id, error)) {
    return false;
  }
  if (!IsValidDeviceId(output->device_id)) return Bad("BOOMPI_DEVICE_ID", error);
  if ((output->server_ip.empty() != output->server_spki_sha256.empty()) ||
      (!output->server_spki_sha256.empty() &&
       !IsValidSpkiSha256(output->server_spki_sha256))) {
    return Bad("BOOMPI_SERVER_SPKI_SHA256", error);
  }
  std::uint32_t parsed = 0U;
  if (!ReadUint("BOOMPI_SERVER_PORT", 17806U, 1U, 65535U, &parsed, error)) return false;
  output->server_port = static_cast<std::uint16_t>(parsed);
  if (!ReadPolarity("BOOMPI_CAPTURE_LEFT_POLARITY", &output->capture_left_polarity, error) ||
      !ReadPolarity("BOOMPI_CAPTURE_RIGHT_POLARITY", &output->capture_right_polarity, error) ||
      !ReadUint("BOOMPI_VOLUME_PERCENT", 60U, 0U, 100U, &parsed, error)) {
    return false;
  }
  output->volume_percent = static_cast<std::uint8_t>(parsed);
  if (!ReadString("BOOMPI_SNOWBOY_SENSITIVITY", false, 32U,
                  &output->snowboy_sensitivity, error)) {
    return false;
  }
  if (output->snowboy_sensitivity.empty()) output->snowboy_sensitivity = "0.7";
  if (!IsSnowboySensitivity(output->snowboy_sensitivity)) return Bad("BOOMPI_SNOWBOY_SENSITIVITY", error);
  if (!ReadFloat("BOOMPI_VAD_MIN_DBFS", -35.0F, -90.0F, 0.0F,
                 &output->vad_min_dbfs, error) ||
      !ReadFloat("BOOMPI_BARGE_MIN_DBFS", -25.0F, -90.0F, 0.0F,
                 &output->barge_min_dbfs, error))
    return false;
  if (error != nullptr) error->clear();
  return true;
}

}  // namespace boompi::config
