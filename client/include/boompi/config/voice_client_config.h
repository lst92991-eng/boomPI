#pragma once
#include <cstdint>
#include <string>

namespace boompi::config {
// 身份由安装脚本生成。固定端点仅供教师部署；声学参数属于板级 profile。
struct VoiceClientConfig final {
  std::string device_id;
  std::string server_ip;
  std::string server_spki_sha256;
  std::uint16_t server_port{17806U};
};
bool IsValidDeviceId(const std::string& value) noexcept;
bool IsValidSpkiSha256(const std::string& value) noexcept;
bool LoadVoiceClientConfigFromEnvironment(VoiceClientConfig* output, std::string* error);
}  // namespace boompi::config
