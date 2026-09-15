#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

#include "boompi/config/voice_client_config.h"

namespace boompi::voice_net {

// Online 表示已收到 ready；Offline 会结束当前轮，网络线程随后尝试重连。
enum class LinkEventKind : std::uint8_t { Online, Offline, Text, Audio, Done, Error };

// poll 交付拥有独立数据的事件，不借用 WebSocket 消息内存。
struct LinkEvent final {
  LinkEventKind kind{LinkEventKind::Error};
  std::uint32_t generation{0};  // 连接事件为 0；回答事件属于对应轮次。
  std::uint32_t sequence{0};  // 下行音频从 0 连续递增，缺帧/重复帧会断开连接。
  std::string text;           // Text：增量字幕。
  std::string code;           // Error/Offline：可打印的短错误码。
  std::array<std::uint8_t, 640> audio{};  // Audio：16 kHz/mono/S16_LE。
  std::size_t audio_size{0};              // 末帧可少于 320 个样本。
};

enum class SendResult : std::uint8_t { Ok, Backpressure, Disconnected };

// 应用线程投递 START、PCM、END 与取消；网络线程拥有真实 WSS 连接。
// Ok 仅表示入队；Backpressure 必须取消本轮，不能跳过 PCM 后补 END。
bool open(const config::VoiceClientConfig& config);
bool poll(LinkEvent* event);
bool online();
bool uploading();
SendResult start(std::uint32_t generation, bool supersede);
SendResult send(std::uint32_t generation, const std::int16_t* pcm);
SendResult end(std::uint32_t generation);
// 用新的 generation 退休旧轮；上传 END 后仍可撤回正在等待或播放的回复。
bool cancel(std::uint32_t new_generation, bool retract);
void close() noexcept;

// 保存后供网络准备使用，不等待联网。SSID 1～32 字节，密码 8～63 字节。
// 拒绝控制字符并转义字段，完整写入后原子替换配置；调用方不得记录凭据。
// 有文件 I/O，只能由主线程/UI 等非实时线程调用。
bool save_wifi(const std::string& ssid, const std::string& password);

}  // namespace boompi::voice_net
