#ifndef BOOMPI_NETWORK_WSS_CLIENT_H_
#define BOOMPI_NETWORK_WSS_CLIENT_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "boompi/event/status.h"
#include "boompi/protocol/audio_packet.h"

namespace boompi::network {

struct WssClientConfig final {
  std::string server_ip;
  std::uint16_t server_port{17806U};
  std::string server_name;
  std::string ca_certificate_path;
  std::string spki_sha256_base64;
  std::uint32_t connect_timeout_ms{5000U};
  std::uint32_t write_timeout_ms{10000U};
  std::uint32_t receive_idle_timeout_ms{30000U};
};

enum class WssInboundMessageType : std::uint8_t {
  kUnset = 0U,
  kControl,
  kPcm24k,
};

struct WssInboundMessage final {
  WssInboundMessageType type{WssInboundMessageType::kUnset};
  std::string control_json;
  protocol::AudioPacketHeader pcm_header{};
  std::vector<std::uint8_t> pcm_payload;
};

// Development transport for the single-turn vertical slice. One network owner
// must serialize Connect/Send/Receive/Close calls; the class starts no threads.
// RequestStop and RequestReceiveInterrupt may be called concurrently.
// RequestStop aborts the connection. RequestReceiveInterrupt only wakes a
// Receive that has not consumed any bytes of its next WebSocket frame, so the
// network owner can send a latency-sensitive control message without breaking
// framing or reconnecting.
class WssClient final {
 public:
  explicit WssClient(WssClientConfig config);
  ~WssClient();

  WssClient(const WssClient&) = delete;
  WssClient& operator=(const WssClient&) = delete;

  Status Connect();
  Status SendControl(const std::string& control_json);
  Status SendPcm(const protocol::AudioPacketHeader& header,
                 const std::uint8_t* payload,
                 std::size_t payload_size_bytes);
  Status SendKeepAlivePong();
  Status Receive(WssInboundMessage* output);
  void RequestReceiveInterrupt() noexcept;
  void RequestStop() noexcept;
  void Close() noexcept;
  bool connected() const noexcept;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

Status ValidateWssClientConfig(const WssClientConfig& config);

}  // namespace boompi::network

#endif  // BOOMPI_NETWORK_WSS_CLIENT_H_
