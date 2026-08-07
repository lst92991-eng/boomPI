#pragma once

/**
 * @file voice_client_config.h
 * @brief 定义板端语音客户端启动配置及其公开校验入口。
 *
 * 配置对象在进程启动阶段构造，随后由各模块按只读方式使用。字段分为课堂部署可调
 * 参数和已经由当前 RV1106 BSP 验证的硬件事实，避免运行期修改设备格式或模型路径。
 */

#include <cstdint>
#include <string>

namespace boompi::config {

/**
 * @brief 语音客户端启动后共享的只读配置快照。
 *
 * 服务端地址与 SPKI pin 同时为空时启用局域网发现；同时给出时连接固定服务端。
 * 音频格式、模型路径和产品功能开关保持板级固定值，课堂只需关注身份、音量和检测阈值。
 */
struct VoiceClientConfig final {
  std::string server_ip, server_spki_sha256;  ///< WSS host/IP 与 base64(SHA-256(SPKI)) pin，必须成对出现。
  std::uint16_t server_port{17806U};           ///< WSS TCP 端口，范围 1..65535。
  std::string device_id;                      ///< 协议中的稳定设备身份：非全零小写 UUID。

  // 当前镜像已经验证这些路径和设备格式。集中保留在配置结构中，便于模块依赖注入；
  // 环境加载器不会覆盖它们，防止课堂误配置破坏四通道采集或模型 ABI。
  std::string capture_pcm{"hw:0,0"}, playback_pcm{"hw:0,0"};
  std::string snowboy_resource_path{"/userdata/boompi/models/common.res"};
  std::string snowboy_model_path{"/userdata/boompi/models/snowboy.umdl"};
  std::uint16_t speaker_gain_percent{100U};  ///< 板级增益系数，与用户音量相乘后交给播放线程。
  bool barge_in_enabled{true};               ///< 当前产品始终支持播放中近讲打断。

  std::uint8_t volume_percent{60U};           ///< 用户音量，闭区间 0..100。
  std::string snowboy_sensitivity{"0.7"};     ///< 当前人工验收的单关键词敏感度，闭区间 0..1。
  float vad_min_dbfs{-30.0F};                 ///< 常规收音准入：原始双麦较强一路，范围 -90..0 dBFS。
  float barge_min_dbfs{-25.0F};               ///< 播放期近讲准入：AEC 后语音，范围 -90..0 dBFS。
  std::int8_t capture_left_polarity{1}, capture_right_polarity{1};  ///< 麦克风样本符号修正，仅允许 +1/-1。
};

/// @brief 校验线协议使用的小写 UUID，并拒绝会丢失设备身份意义的全零值。
bool IsValidDeviceId(const std::string& value) noexcept;

/// @brief 校验 SHA-256 SPKI 指纹的标准 44 字符 base64 编码。
bool IsValidSpkiSha256(const std::string& value) noexcept;

/// @brief 从环境变量加载并完整校验不依赖外部 I/O 的启动配置。
///
/// 本函数只加载学生可能需要调整的参数。ALSA 设备、模型路径、板级增益和
/// barge-in 开关由当前 BSP 固定。校验成功后，调用方可以直接把同一快照传给
/// application、音频和网络模块。
///
/// @param output 非空输出对象；错误文本只包含变量名而不包含秘密值。
/// @return 全部字段满足边界时返回 true；指针为空、缺项或字段越界时返回 false。
bool LoadVoiceClientConfigFromEnvironment(VoiceClientConfig* output,
                                          std::string* error);

}  // namespace boompi::config
