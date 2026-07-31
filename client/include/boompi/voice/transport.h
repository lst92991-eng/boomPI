#ifndef BOOMPI_VOICE_TRANSPORT_H_
#define BOOMPI_VOICE_TRANSPORT_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace boompi::voice {

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
  std::uint64_t timestamp_us{0};
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
  std::uint32_t connect_timeout_ms{5000};
  std::size_t maximum_message_bytes{64U * 1024U + 64U};
  std::size_t maximum_buffered_bytes{128U * 1024U};
};

using InboundHandler = std::function<void(Inbound)>;
using ErrorHandler = std::function<void(std::string)>;
using CloseHandler = std::function<void(std::uint16_t, std::string)>;

class Transport final {
 public:
  Transport();
  ~Transport();
  Transport(const Transport&) = delete;
  Transport& operator=(const Transport&) = delete;

  // Starts the WebSocket service thread without waiting for the handshake.
  // Poll connected(); failures are reported through the callbacks.
  bool Connect(TransportConfig config, InboundHandler inbound,
               ErrorHandler error, CloseHandler closed);
  bool SendControl(const Control& control);
  bool SendPcm64(const Pcm64Header& header, const std::uint8_t* payload,
                 std::size_t bytes);
  void Close() noexcept;
  bool connected() const noexcept;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace boompi::voice

#endif  // BOOMPI_VOICE_TRANSPORT_H_
