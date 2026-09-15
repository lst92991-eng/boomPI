#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "boompi/audio/audio_format.h"
#include "boompi/config/voice_client_config.h"

namespace boompi::voice_net {

// Online 表示已收到 ready；Offline 会结束当前轮，网络线程随后尝试重连。
enum class LinkEventKind : std::uint8_t { Online, Offline, Text, Audio, Done, Error };

// poll 交付拥有独立数据的事件，不借用 WebSocket 消息内存。
struct LinkEvent final {
  LinkEventKind kind{LinkEventKind::Error};
  std::uint32_t generation{0};  // 连接事件为 0；回答事件属于对应轮次。
  std::uint32_t sequence{0};  // 下行音频从 0 连续递增，缺帧/重复帧会断开连接。
  // TEXT为UTF-8，ERROR/Offline为短错误码，AUDIO为S16_LE字节；每个事件只有一份负载。
  std::string data;
};

enum class SendResult : std::uint8_t { Ok, Backpressure, Disconnected };

// 应用线程投递 START、PCM、END 与取消；网络线程拥有真实 WSS 连接。
// Ok 仅表示入队；Backpressure 必须取消本轮，不能跳过 PCM 后补 END。
// 配置由LoadClientConfig校验；发现/缓存端点由网络准备模块校验，TLS仍验证实际公钥。
bool open(const config::VoiceClientConfig& config);
bool poll(LinkEvent& event);
bool online();
bool uploading();
SendResult start(bool supersede);
SendResult send(const audio::VoiceFrame16k& pcm);
SendResult end();
// 网络分配新的generation退休旧轮；END后仍可撤回等待/播放中的回复。
bool cancel(bool retract);
void close() noexcept;

// 保存后供网络准备使用，不等待联网。SSID 1～32 字节，密码 8～63 字节。
// 拒绝控制字符并转义字段，完整写入后原子替换配置；调用方不得记录凭据。
// 有文件 I/O，只能由主线程/UI 等非实时线程调用。
bool save_wifi(const std::string& ssid, const std::string& password);

}  // namespace boompi::voice_net
