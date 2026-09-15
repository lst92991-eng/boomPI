#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "boompi/config/voice_client_config.h"

namespace boompi::network {

// Online 表示已收到 ready；Offline 会结束当前轮，网络线程随后尝试重连。
enum class LinkEventKind : std::uint8_t { Online, Offline, Text, Audio, Done, Error };

// PollEvent 交付拥有独立数据的事件，不借用 WebSocket 消息内存。
struct LinkEvent final {
  LinkEventKind kind{LinkEventKind::Error};
  std::uint32_t generation{0};  // 连接事件为 0；回答事件属于对应轮次。
  std::uint32_t sequence{0};  // 下行音频从 0 连续递增，缺帧/重复帧会断开连接。
  bool start{false};
  bool end{false};
  std::string text;                       // Text：增量字幕。
  std::string code;                       // Error/Offline：可打印的短错误码。
  std::array<std::uint8_t, 640> audio{};  // Audio：16 kHz/mono/S16_LE。
  std::size_t audio_size{0};              // 末帧可少于 320 个样本。
};

enum class SendResult : std::uint8_t { Ok, Backpressure, Disconnected };

// 应用线程调用本类；一个网络线程负责 DHCP/发现、TLS、握手、收发、心跳与重连。
// 两个线程通过有界队列交接。generation 由应用分配，旧轮次不会进入新问题。
// 服务端是配套黑箱；客户端只依赖 protocol/protocol-v2.md 定义的输入输出。
class VoiceLink final {
 public:
  VoiceLink();
  ~VoiceLink();
  VoiceLink(const VoiceLink&) = delete;
  VoiceLink& operator=(const VoiceLink&) = delete;

  // 复用进程配置。成功仅表示线程启动；收到 Online 后才能投递音频。
  // 地址和 SPKI 都空时自动发现；显式地址要求 IPv4、有效端口与 SPKI 配套。
  bool Open(const config::VoiceClientConfig& config);
  // 非阻塞地取最早事件；队列空或 event 为空时返回 false，不修改输出。
  bool PollEvent(LinkEvent* event);
  // 复制 320 个 S16 样本（16 kHz/mono/20 ms），内部生成连续包序号。
  // 首帧 start 的 generation 必须递增；续帧属于当前轮，end 后不再接受该轮 PCM。
  // supersede 只用于首帧，通知服务端退休并撤回旧回答，无须等待 cancel ACK。
  // Ok 仅表示入队。Backpressure 表示未接收，必须 Stop，不能补 END 掩盖缺帧。
  SendResult SendAudio(std::uint32_t generation, const std::int16_t* pcm, bool start, bool end,
                       bool supersede);
  // 以递增的新 generation 退休旧轮次，本代不产生回答；retract 决定是否撤回历史。
  // 成功仅表示控制帧入队；失败时应用转离线。
  bool Stop(std::uint32_t new_generation, bool retract);
  // 请求退出、join 网络线程，再清队列；不能从网络线程调用，允许重复调用。
  void Close() noexcept;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

// 保存后供网络准备使用，不等待联网。SSID 1～32 字节，密码 8～63 字节。
// 拒绝控制字符并转义字段，完整写入后原子替换配置；调用方不得记录凭据。
// 有文件 I/O，只能由主线程/UI 等非实时线程调用。
bool SaveWifi(const std::string& ssid, const std::string& password);

}  // namespace boompi::network
