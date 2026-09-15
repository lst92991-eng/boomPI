#pragma once

/**
 * @file voice_codec.h
 * @brief 网络模块私有的 v2 单帧编解码声明。
 *
 * VoiceLink 发送时 EncodeAudio，接收时按 WebSocket opcode 选择 DecodeText/DecodeAudio。
 * 函数不持有连接或跨帧状态；应用仅接触 voice_link.h，不直接拼装这些线协议字节。
 */

#include "boompi/network/voice_link.h"

namespace boompi::network::detail {

/// magic/保留位/flags/generation/sequence 共 16 字节，整数头字段采用网络大端序。
constexpr std::size_t kHeaderBytes = 16;
/// 双向 PCM：16 kHz × 20 ms × 单声道 × 2 字节；只有下行 END 可短于此长度。
constexpr std::size_t kPcmBytes = 640;
constexpr std::size_t kFrameBytes = kHeaderBytes + kPcmBytes;
/**
 * @brief 严格解析 ready/text/done/error 控制帧，返回可移交应用的事件值。
 * @param json 完整 WebSocket 文本负载，最大 8192 字节；只接受协议约定字段。
 * @throws std::runtime_error 结构、UTF-8、字段或数值不合法，错误文本不包含原始消息。
 */
LinkEvent DecodeText(const std::string& json);
/**
 * @brief 检查下行头与 PCM 长度，将 16 kHz S16_LE 字节复制到 Audio 事件。
 *
 * 普通帧为 640 字节 PCM；带 END 的末帧允许 2～640 字节且必须 sample 对齐。
 * START 必须对应 sequence 0；跨帧 generation 归属与 sequence 连续性由 VoiceLink 检查。
 * @throws std::runtime_error 单帧违反协议时抛出异常，VoiceLink 将连接置为故障。
 */
LinkEvent DecodeAudio(const std::string& bytes);
/**
 * @brief 把 320 个有符号 sample 转成 656 字节的固定上行帧。
 *
 * 调用方须先检查非空 pcm、代号递增及 flags 组合；本函数只编码，不重复做轮次校验。
 * 返回数组拥有全部字节，主机端序和原始 pcm 生命周期不影响随后排队发送。
 */
std::array<std::uint8_t, kFrameBytes> EncodeAudio(std::uint32_t generation,
                                                  std::uint32_t sequence,
                                                  const std::int16_t* pcm, bool start, bool end,
                                                  bool supersede);

}  // namespace boompi::network::detail
