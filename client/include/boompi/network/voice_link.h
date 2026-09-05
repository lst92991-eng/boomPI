#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace boompi::network {

struct LinkConfig final {
  std::string device_id;
  std::string host;  // 留空时自动发现；显式端点仅用于教师诊断。
  std::uint16_t port{17806};
  std::string spki;
};

enum class LinkEventKind : std::uint8_t { Online, Offline, Text, Audio, Done, Error };

struct LinkEvent final {
  LinkEventKind kind{LinkEventKind::Error};
  std::uint32_t generation{0};
  std::uint32_t sequence{0};
  bool start{false};
  bool end{false};
  std::string text;
  std::string code;
  std::array<std::uint8_t, 960> audio{};
  std::size_t audio_size{0};
};

enum class SendResult : std::uint8_t { Ok, Backpressure, Disconnected };

// 一个网络线程拥有建链、TLS、握手、心跳和重连。应用只分配 generation，
// 不处理连接内部状态。跨线程队列有界，旧 generation 不能污染新问题。
class VoiceLink final {
 public:
  VoiceLink();
  ~VoiceLink();
  VoiceLink(const VoiceLink&) = delete;
  VoiceLink& operator=(const VoiceLink&) = delete;

  bool Open(const LinkConfig& config);
  bool Poll(LinkEvent* event);
  // pcm 必须指向320个S16 sample。Ok表示已复制入队，不等待网络线程；
  // Backpressure 表示本帧未接收，应用须 Stop，不能伪造END提交缺帧输入。
  SendResult SendAudio(std::uint32_t generation, const std::int16_t* pcm,
                       bool start, bool end, bool supersede);
  // 新generation立即退休旧回复/旧队列；保留槽无法发送时整条连接失效。
  bool Stop(std::uint32_t new_generation, bool retract);
  void Close() noexcept;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

bool SaveWifi(const std::string& ssid, const std::string& password);

}  // namespace boompi::network
