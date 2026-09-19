/**
 * @file voice_client_config.h
 * @brief 读取和保存设备身份、已配对服务端端点。
 *
 * App_Init 读取配置后交给网络模块；地址和 TLS pin
 * 首次留空时自动发现，后续发现须匹配已保存公钥。音频格式/门限属于板级 profile，API Key 仅存在于配套服务端。
 */
#pragma once
#include <cstdint>
#include <string>
#include <string_view>

namespace config
{
/** @brief 程序首次创建配置时生成设备身份；教师可预置地址和SPKI完成课堂配对。 */
struct VoiceClientConfig final
{
    /// 非全零、小写十六进制 UUID 文本，用于协议 hello 中的设备身份。
    std::string device_id;
    /// 最近一次配对地址；空串表示尚未配对，非空时为标准IPv4地址。
    std::string server_ip;
    /// TCP 端口；未设置时使用配套服务默认值。
    std::uint16_t server_port{17806U};
    /// 服务端公钥 SPKI 的 SHA-256/Base64 指纹；与 server_ip 同时为空或同时设置。
    std::string server_spki_sha256;
};
// 仅传入已校验的配置；网络线程在地址变化时保存，成功后才能使用新端点。
bool SaveClientConfig(const VoiceClientConfig &settings);
/// 检查 UUID 文本形状、大小写及非全零约束；不核验 UUID 版本/变体位或全网唯一性。
bool IsValidDeviceId(std::string_view value);
/// 检查 32 字节摘要的规范 Base64 形式；这里只校验格式，TLS 握手时才验证远端公钥。
bool IsValidSpkiSha256(std::string_view value);
/**
 * @brief 读取client.conf；缺失时创建设备身份，再校验地址/pin配对和端口。
 * @param output 非空；成功时包含已校验配置，失败时不可用于启动。
 * @param error 可选错误输出，只报告字段名称；成功时清空。
 * @return 配置全部通过返回 true；不进行网络连接、证书认证或设备探测。
 */
bool LoadClientConfig(VoiceClientConfig *output, std::string *error);
}  // namespace config
