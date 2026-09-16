/**
 * @file voice_client_config.h
 * @brief 进程启动前读取并校验设备身份与可选服务端端点。
 *
 * main 校验配置后交给 App_Init，网络直接复用同一份配置；地址和 TLS pin
 * 留空时使用自动发现。音频格式/门限属于板级 profile，API Key 仅存在于配套服务端。
 */
#pragma once
#include <cstdint>
#include <string>
#include <string_view>

namespace boompi::config {
/** @brief 程序首次创建配置时生成设备身份；可预置地址和SPKI来固定课堂端点。 */
struct VoiceClientConfig final {
  /// 非全零、小写十六进制 UUID 文本，用于协议 hello 中的设备身份。
  std::string device_id;
  /// 空串表示发现模式；非空时必须为标准十进制 IPv4 地址。
  std::string server_ip;
  /// TCP 端口；未设置时使用配套服务默认值。
  std::uint16_t server_port{17806U};
  /// 服务端公钥 SPKI 的 SHA-256/Base64 指纹；与 server_ip 同时为空或同时设置。
  std::string server_spki_sha256;
};
/// 检查 UUID 文本形状、大小写及非全零约束；不核验 UUID 版本/变体位或全网唯一性。
bool IsValidDeviceId(std::string_view value) noexcept;
/// 检查 32 字节摘要的规范 Base64 形式；这里只校验格式，TLS 握手时才验证远端公钥。
bool IsValidSpkiSha256(std::string_view value) noexcept;
/**
 * @brief 读取client.conf；缺失时创建设备身份，再校验地址/pin配对和端口。
 * @param output 非空；全部校验成功后一次赋值，失败保留默认值，不可用于启动。
 * @param error 可选错误输出，只报告字段名称；成功时清空。
 * @return 配置全部通过返回 true；不进行网络连接、证书认证或设备探测。
 */
bool LoadClientConfig(VoiceClientConfig* output, std::string* error);
}  // namespace boompi::config
