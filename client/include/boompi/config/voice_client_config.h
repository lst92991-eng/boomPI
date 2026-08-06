#pragma once

#include <cstdint>
#include <string>

namespace boompi::config {

/// @brief 语音客户端启动后按只读方式使用的进程级配置。
///
struct VoiceClientConfig final {
  std::string server_ip, server_spki_sha256;  ///< WSS host/IP 与合法 base64(SHA-256(SPKI)) pin。
  std::uint16_t server_port{17806U};           ///< WSS TCP 端口，范围 1..65535。
  std::string device_id;                      ///< 非全零小写 UUID。
  // 下面四项是当前 BSP 已验证的硬件事实，不开放成教学配置项。
  std::string capture_pcm{"hw:0,0"}, playback_pcm{"hw:0,0"};
  std::string snowboy_resource_path{"/userdata/boompi/models/common.res"};
  std::string snowboy_model_path{"/userdata/boompi/models/snowboy.umdl"};
  std::uint16_t speaker_gain_percent{100U};
  bool barge_in_enabled{true};

  std::uint8_t volume_percent{60U};           ///< 用户音量，闭区间 0..100。
  std::string snowboy_sensitivity{"0.7"};     ///< 当前人工验收的单关键词敏感度，闭区间 0..1。
  float vad_min_dbfs{-30.0F};                 ///< 原始双麦较强一路的 VAD 能量门限，范围 -90..0 dBFS。
  float barge_min_dbfs{-25.0F};               ///< AEC 后语音打断门限，范围 -90..0 dBFS。
  std::int8_t capture_left_polarity{1}, capture_right_polarity{1};  ///< 仅允许 +1/-1。
};

/// WSS 连接和 --check-config 共用的设备身份格式边界。
bool IsValidDeviceId(const std::string& value) noexcept;
bool IsValidSpkiSha256(const std::string& value) noexcept;

/// @brief 从环境变量加载并完整校验不依赖外部 I/O 的启动配置。
///
/// 本函数只加载学生可能需要调整的参数。ALSA 设备、模型路径、板级增益和
/// barge-in 开关属于当前 BSP 的固定事实，不读取同名环境变量。
///
/// @param output 非空输出对象；错误文本只包含变量名而不包含秘密值。
/// @return 前置校验通过时返回成功；指针为空、缺项或字段越界时返回参数错误。
bool LoadVoiceClientConfigFromEnvironment(VoiceClientConfig* output,
                                          std::string* error);

}  // namespace boompi::config
