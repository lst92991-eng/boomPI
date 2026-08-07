#pragma once

/**
 * @file voice_transport.h
 * @brief boomPI v1 持久 WSS 的类型边界与板端传输接口。
 *
 * 控制事件使用 JSON 文本帧，实时语音使用固定 64 字节头加 PCM 的二进制帧。
 * 本接口负责单帧编解码、TLS 身份校验和有界发送；跨帧会话顺序由 application
 * actor 统一拥有，避免网络线程形成第二套 turn/response 状态机。
 */

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace boompi::network {

/**
 * @brief 一帧媒体或控制事件所属的会话四元组。
 *
 * session 标识本次 WSS 会话，turn 标识一次用户发言，stream 标识该次媒体流，
 * epoch 标识取消或重连后的代次。application actor 每次开启新代次并校验入站
 * 四元组，使网络中迟到的旧回答无法修改当前对话。
 */
struct WireIds final {
  std::uint32_t session{0};
  std::uint32_t turn{0};
  std::uint32_t stream{0};
  std::uint32_t epoch{0};
};

/// 客户端能够发送的 v1 控制帧全集；payload 由实现按协议生成，调用方只选语义。
enum class ControlKind : std::uint8_t {
  kHello,
  kTurnStart,
  kTurnCommit,
  kTurnCancel,
  kResponseCancel,
};

/// 一条上行控制事件；message_id 用于日志关联，ids 由 application actor 分配。
struct Control final {
  ControlKind kind{ControlKind::kHello};
  std::string message_id;
  WireIds ids;
};

/**
 * @brief 64 字节 PCM wire header 中由 application 提供的动态字段。
 *
 * flags 的 bit0/bit1/bit2 分别表示 START、END、DISCONTINUITY。sequence 在单个
 * stream 内从 0 连续递增；发送失败时调用方不能前移它，否则服务端会观察到
 * 无法解释的音频空洞。
 */
struct Pcm64Header final {
  std::uint16_t flags{0};
  std::uint32_t sequence{0};
  std::uint64_t timestamp_us{0};  ///< 进程 monotonic 起点上的微秒时间戳，与 UTC 无关。
  WireIds ids;
};

/**
 * @brief 上行 PCM 的三态结果。
 *
 * kBackpressure 表示 WSS 已连接但有界发送水位已满，调用方应取消当前 turn；
 * kRejected 表示参数、连接或发送线程不可用。分开表达两者可以避免局部拥塞
 * 被 application 错误放大为整条 TLS 会话重建。
 */
enum class SendOutcome : std::uint8_t { kOk, kBackpressure, kRejected };

/// WSS 线程解码后交给 application actor 的事件种类。
enum class InboundKind : std::uint8_t {
  kHelloAck, kHeartbeat,
  kResponseStart,
  kTextDelta,
  kAudioStart,
  kCancelled,
  kDone,
  kError,
  kPcm,
};

/**
 * @brief 一条已完成结构校验的入站事件。
 *
 * 控制事件使用 response_id/text/error_code；PCM 事件使用 pcm/audio/audio_size。
 * ids 随所有会话相关事件传递，application actor 仍需根据当前 epoch、response
 * 和期望 sequence 做跨帧授权。固定音频数组让 ASIO 回调无需逐帧堆分配。
 */
struct Inbound final {
  // Qwen 24 kHz mono 的 20 ms PCM 为 960 bytes；固定数组避免 WSS 回调逐帧分配音频。
  static constexpr std::size_t kMaximumPcmBytes = 960U;
  InboundKind kind{InboundKind::kError};
  WireIds ids;
  std::string response_id;
  std::string text;
  std::string error_code;
  Pcm64Header pcm;
  std::array<std::uint8_t, kMaximumPcmBytes> audio{};
  std::size_t audio_size{0U};
};

/// 建立 WSS 所需的地址、稳定设备 UUID 与发现阶段保存的服务端 SPKI pin。
struct TransportConfig final {
  std::string host;
  std::uint16_t port{17806};
  std::string device_id;
  std::string spki_sha256_base64;
};

/// 两类回调均在唯一 ASIO 线程执行；实现必须快速投递，不能执行 UI 或音频阻塞操作。
using InboundHandler = std::function<void(Inbound)>;
using ErrorHandler = std::function<void(std::string)>;

/// @brief 持久 WSS 连接、boomPI v1 帧编解码与有界发送。
///
/// Connect 创建唯一 ASIO 线程，连接成功后持续承载 hello、多个 turn 和多段 TTS，
/// 断线时由 application 决定何时重建实例。回调只能把 Inbound 投递给 application；
/// turn/epoch/response/sequence 的唯一 owner 是 application actor。
class VoiceTransport final {
 public:
  VoiceTransport(); ~VoiceTransport();
  VoiceTransport(const VoiceTransport&) = delete; VoiceTransport& operator=(const VoiceTransport&) = delete;

  /// @brief 启动 WSS 线程但不等待握手；调用方轮询 connected。
  ///
  /// 参数、URI 或线程创建失败会同步返回 false；启动后的连接失败由 error/closed 回调报告。
  /// 三个回调都在 ASIO 线程执行，必须快速返回；同一个实例只允许调用一次 Connect。
  bool Connect(TransportConfig config, InboundHandler inbound,
               ErrorHandler error);

  /// @brief 按 v1 固定 schema 编码并排队一个控制事件；会话顺序由调用方保证。
  bool SendControl(const Control& control);

  /**
   * @brief 校验并排队一帧 16 kHz mono PCM。
   *
   * payload 在返回前复制到单槽跨线程缓冲，调用方可以立即复用采集帧。每次调用
   * 得到确定的发送、背压或拒绝结果，application 据此维护 sequence 连续性。
   */
  SendOutcome SendPcm64(const Pcm64Header& header,
                        const std::uint8_t* payload, std::size_t bytes);

  /// Close 幂等停止 ASIO 并回收线程；connected 是跨线程原子快照。心跳由服务端驱动。
  void Close() noexcept; bool connected() const noexcept;

 private:
  // Pimpl 隔离 WebSocket++/OpenSSL 头和线程状态，公开头只保留教学所需的协议边界。
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace boompi::network
