#ifndef BOOMPI_CONFIG_VOICE_CLIENT_CONFIG_H_
#define BOOMPI_CONFIG_VOICE_CLIENT_CONFIG_H_

#include <cstdint>
#include <string>

#include "boompi/event/status.h"

namespace boompi::config {

/// @brief 语音客户端启动后按只读方式使用的进程级配置。
///
/// `device_token` 是秘密字段，析构函数只尽力覆写本对象当前持有的字符串缓冲区；环境变量、
/// 协议编码产生的临时副本及第三方库副本不由本对象清理。日志和文档禁止输出令牌值。
struct VoiceClientConfig final {
  VoiceClientConfig() = default;
  ~VoiceClientConfig() noexcept;

  VoiceClientConfig(const VoiceClientConfig&) = delete;
  VoiceClientConfig& operator=(const VoiceClientConfig&) = delete;

  std::string server_ip, server_spki_sha256;  ///< WSS host/IP 与合法 base64(SHA-256(SPKI)) pin。
  std::uint16_t server_port{17806U};           ///< WSS TCP 端口，范围 1..65535。
  std::string device_id, device_token;        ///< 非全零小写 UUID；32..256 字节可打印 ASCII 令牌。
  std::string capture_pcm, playback_pcm;      ///< 当前镜像实测的 ALSA PCM 标识，最长 31 字节。
  std::uint8_t volume_percent{60U};           ///< 用户音量，闭区间 0..100。
  std::uint16_t speaker_gain_percent{100U};   ///< 板级补偿百分比，范围 1..400；与用户音量相乘。
  std::string snowboy_resource_path, snowboy_model_path;  ///< 最长 1024 字节；voice loop 打开时必须非空。
  std::string snowboy_sensitivity{"0.5"};     ///< 单关键词敏感度，十进制闭区间 0..1。
  std::int8_t capture_left_polarity{1}, capture_right_polarity{1};  ///< 仅允许 +1/-1。
  bool barge_in_enabled{true};                ///< true 时允许播放期间用近讲证据触发打断。
};

/// @brief 从环境变量加载并完整校验不依赖外部 I/O 的启动配置。
///
/// 本函数验证 Snowboy 路径非空、单关键词敏感度和 SPKI pin 编码；文件是否存在、ALSA PCM
/// 是否可打开仍只能在 voice loop 启动阶段确认。
///
/// @param output 非空输出对象。成功后持有令牌的一份副本；失败时仍可安全析构。原环境变量
///               及后续协议副本不属于该对象，错误文本只包含变量名而不包含秘密值。
/// @return 前置校验通过时返回成功；指针为空、缺项或字段越界时返回参数错误。
Status LoadVoiceClientConfigFromEnvironment(VoiceClientConfig* output);

}  // namespace boompi::config

#endif  // BOOMPI_CONFIG_VOICE_CLIENT_CONFIG_H_
