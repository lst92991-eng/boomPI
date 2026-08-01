#ifndef BOOMPI_NETWORK_VOICE_TRANSPORT_H_
#define BOOMPI_NETWORK_VOICE_TRANSPORT_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace boompi::network {

/// 四元组是所有 turn/response 数据的身份边界；epoch 变化后旧包必须丢弃。
struct WireIds final {
  std::uint32_t session{0};
  std::uint32_t turn{0};
  std::uint32_t stream{0};
  std::uint32_t epoch{0};
};

enum class ControlKind : std::uint8_t {
  kHello,
  kTurnStart,
  kTurnCommit,
  kTurnCancel,
  kResponseCancel,
};

struct Control final {
  ControlKind kind{ControlKind::kHello};
  std::string message_id;
  WireIds ids;
  std::string device_token;
  std::uint32_t sample_rate_hz{16000};
};

struct Pcm64Header final {
  std::uint8_t kind{1};
  std::uint16_t flags{0};
  std::uint32_t sample_rate_hz{16000};
  std::uint32_t sequence{0};
  std::uint64_t timestamp_us{0};  ///< 进程 monotonic 起点上的微秒时间戳，不是 UTC。
  WireIds ids;
};

enum class InboundKind : std::uint8_t {
  kHelloAck,
  kResponseStart,
  kTextDelta,
  kAudioStart,
  kCancelled,
  kDone,
  kError,
  kPcm,
};

struct Inbound final {
  // Qwen 24 kHz mono 的 20 ms PCM 为 960 bytes；固定数组避免 WSS 回调逐帧分配音频。
  static constexpr std::size_t kMaximumPcmBytes = 960U;
  InboundKind kind{InboundKind::kError};
  WireIds ids;
  std::string response_id;
  std::string text;
  std::string reason;
  std::string error_code;
  std::uint32_t input_sample_rate_hz{0};
  std::uint32_t output_sample_rate_hz{0};
  std::uint32_t input_frame_ms{0};
  Pcm64Header pcm;
  std::array<std::uint8_t, kMaximumPcmBytes> audio{};
  std::size_t audio_size{0U};
};

struct TransportConfig final {
  std::string host;
  std::uint16_t port{17806};
  std::string device_id;
  std::string spki_sha256_base64;
  std::uint32_t connect_timeout_ms{5000};  ///< TCP/TLS/WSS open 握手上限，单位毫秒。
  std::size_t maximum_message_bytes{64U * 1024U + 64U};
  // 约 800 ms 的 16 kHz/20 ms 上行帧；网络阻塞时不得积压数秒陈旧语音。
  std::size_t maximum_buffered_bytes{28U * 1024U};
};

using InboundHandler = std::function<void(Inbound)>;
using ErrorHandler = std::function<void(std::string)>;
using CloseHandler = std::function<void(std::uint16_t, std::string)>;

/// @brief 持久 WSS 连接及 boomPI v1 有界会话、identity 与 sequence 校验。
///
/// Connect 创建唯一 ASIO 线程。回调在该线程执行，只能把 Inbound 投递给 application；
/// SendControl/SendPcm64 可由 application actor 调用，内部串行化协议状态和 websocket send。
class VoiceTransport final {
 public:
  VoiceTransport(); ~VoiceTransport();
  VoiceTransport(const VoiceTransport&) = delete; VoiceTransport& operator=(const VoiceTransport&) = delete;

  /// @brief 启动 WSS 线程但不等待握手；调用方轮询 connected。
  ///
  /// 参数、URI 或线程创建失败会同步返回 false；启动后的连接失败由 error/closed 回调报告。
  /// 三个回调都在 ASIO 线程执行，必须快速返回；同一个实例只允许调用一次 Connect。
  bool Connect(TransportConfig config, InboundHandler inbound,
               ErrorHandler error, CloseHandler closed);

  /// @brief 校验并排队一个控制事件；成功只表示已交给 websocketpp，不表示对端已处理。
  bool SendControl(const Control& control);

  /// @brief 校验四元组/sequence 后排队一帧 16 kHz mono PCM；payload 在返回前完成复制。
  bool SendPcm64(const Pcm64Header& header, const std::uint8_t* payload,
                 std::size_t bytes);

  /// Close 幂等停止 ASIO；connected 是跨线程原子快照，不代表下一次 send 必然成功。
  void Close() noexcept; bool connected() const noexcept;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace boompi::network

#endif  // BOOMPI_NETWORK_VOICE_TRANSPORT_H_
