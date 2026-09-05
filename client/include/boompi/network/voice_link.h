#pragma once

/**
 * @file voice_link.h
 * @brief 应用使用的网络边界：投递语音、停止旧轮次、取得回答事件。
 */

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace boompi::network {

/// @brief 留空端点时自动发现；显式端点供教师诊断，仍必须验证 SPKI。
struct LinkConfig final {
  std::string device_id;
  std::string host;
  std::uint16_t port{17806};
  std::string spki;
};

enum class LinkEventKind : std::uint8_t {
  Online,
  Offline,
  Text,
  Audio,
  Done,
  Error,
};

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

/**
 * @brief 一个网络线程持有连接，应用只分配 generation 并消费事件。
 *
 * 建链、TLS、握手、心跳和重连留在实现内；投递接口复制数据到有界队列，
 * 不等待 socket 写完。旧 generation 的事件不能进入新问题。
 */
class VoiceLink final {
 public:
  VoiceLink();
  ~VoiceLink();
  VoiceLink(const VoiceLink&) = delete;
  VoiceLink& operator=(const VoiceLink&) = delete;

  bool Open(const LinkConfig& config);
  bool Poll(LinkEvent* event);
  /**
   * @brief 复制一帧 20 ms 音频；pcm 指向 320 个 S16 sample。
   * @return Ok 表示入队；Backpressure 表示本帧未接收，应用必须 Stop，
   *         不能补发 END 把缺帧输入当作完整语句提交。
   */
  SendResult SendAudio(std::uint32_t generation, const std::int16_t* pcm, bool start, bool end,
                       bool supersede);
  /// @brief 用新 generation 退休旧轮次；控制帧无法入队时使连接失效。
  bool Stop(std::uint32_t new_generation, bool retract);
  void Close() noexcept;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

/// @brief 配网页保存 Wi-Fi 凭据；涉及文件写入，只能从非实时线程调用。
bool SaveWifi(const std::string& ssid, const std::string& password);

}  // namespace boompi::network
