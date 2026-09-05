/**
 * @file voice_link.cpp
 * @brief 持久 WSS 连接：线程内完成建链、握手、收发、心跳和重连。
 *
 * 应用线程只投递语音和 STOP，或取走协议事件。共享队列与 generation 用同一把锁
 * 保护，WebSocket++ 和 TLS 对象只由网络线程访问。
 */
#include "boompi/network/voice_link.h"

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>
#include <websocketpp/client.hpp>
#include <websocketpp/config/asio_client.hpp>

#include "boompi/config/voice_client_config.h"
#include "network_setup.h"
#include "voice_codec.h"

namespace boompi::network {

class VoiceLink::Impl final {
  using Clock = std::chrono::steady_clock;
  using Client = websocketpp::client<websocketpp::config::asio_tls_client>;
  using Hdl = websocketpp::connection_hdl;
  using TlsContext = websocketpp::lib::asio::ssl::context;

  static constexpr std::size_t kQueueFrames = 800 / 20;
  // buffered_amount 不包含正在 async_write 的帧。固定预留两帧容量，覆盖正在写的
  // 一帧和已交给 WebSocket++ 排队的一帧，不能把这部分误算为“队列已空”。
  static constexpr std::size_t kTransportFrames = 2;
  static constexpr std::size_t kEventCapacity = 64;
  static constexpr char kTeachingToken[] = "boompi-teaching-shared-token-v1-2026";

  struct Outbound final {
    std::array<std::uint8_t, detail::kFrameBytes> bytes{};
    std::size_t size{0};
    bool text{false};
    bool retirement{false};
    Clock::time_point queued_at{Clock::now()};
  };

 public:
  ~Impl() {
    Close();
  }

  bool Open(const LinkConfig& settings) {
    if (thread_.joinable() || !config::IsValidDeviceId(settings.device_id)) {
      return false;
    }
    if (!settings.host.empty()) {
      websocketpp::lib::asio::error_code error;
      const auto address =
          websocketpp::lib::asio::ip::address::from_string(settings.host, error);
      if (error || !address.is_v4() || settings.port == 0 ||
          !config::IsValidSpkiSha256(settings.spki)) {
        return false;
      }
    } else if (!settings.spki.empty()) {
      return false;
    }
    configured_ = settings;
    stop_.store(false);
    try {
      thread_ = std::thread(&Impl::Run, this);
    } catch (...) {
      return false;
    }
    return true;
  }

  bool Poll(LinkEvent* event) {
    if (event == nullptr) {
      return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (events_.empty()) {
      return false;
    }
    *event = std::move(events_.front());
    events_.pop_front();
    return true;
  }

  SendResult SendAudio(std::uint32_t generation, const std::int16_t* pcm, bool start, bool end,
                       bool supersede) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!online_ || failed_ || stop_.load()) {
      return SendResult::Disconnected;
    }

    bool valid_turn = false;
    if (start) {
      valid_turn = generation > generation_;
    } else {
      valid_turn = generation == generation_ && active_ && !input_ended_ &&
                   uplink_sequence_ != UINT32_MAX;
    }
    if (pcm == nullptr || generation == 0 || !valid_turn || (supersede && !start)) {
      FailLocked("invalid_uplink");
      return SendResult::Disconnected;
    }

    // 普通音频满时把背压交给应用处理；打断先退休旧 PCM，再检查保留下来的控制帧。
    if (!supersede && outbound_.size() + kTransportFrames >= kQueueFrames) {
      return SendResult::Backpressure;
    }
    if (start) {
      AdvanceLocked(generation, true);
    }
    if (outbound_.size() + kTransportFrames >= kQueueFrames) {
      FailLocked("retirement_overflow");
      return SendResult::Disconnected;
    }

    Outbound frame;
    frame.retirement = supersede;
    frame.bytes = detail::EncodeAudio(generation, uplink_sequence_, pcm, start, end, supersede);
    frame.size = frame.bytes.size();
    outbound_.push_back(std::move(frame));
    ++uplink_sequence_;
    input_ended_ = end;
    changed_.notify_one();
    return SendResult::Ok;
  }

  bool Stop(std::uint32_t generation, bool retract) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!online_ || failed_ || stop_.load()) {
      return false;
    }
    if (generation == 0 || generation <= generation_) {
      FailLocked("invalid_stop");
      return false;
    }
    AdvanceLocked(generation, false);
    if (outbound_.size() >= kQueueFrames) {
      FailLocked("retirement_overflow");
      return false;
    }

    std::string text =
        "{\"type\":\"stop\",\"generation\":" + std::to_string(generation) + ",\"retract\":";
    if (retract) {
      text += "true}";
    } else {
      text += "false}";
    }
    Outbound frame;
    frame.text = true;
    frame.retirement = true;
    frame.size = text.size();
    std::copy(text.begin(), text.end(), frame.bytes.begin());
    outbound_.push_back(std::move(frame));
    changed_.notify_one();
    return true;
  }

  void Close() noexcept {
    stop_.store(true);
    changed_.notify_all();
    if (thread_.joinable()) {
      thread_.join();
    }
    std::lock_guard<std::mutex> lock(mutex_);
    online_ = false;
    events_.clear();
    outbound_.clear();
  }

 private:
  /**
   * @brief 在锁内切代，让新 START 与旧事件过滤使用同一个边界。
   *
   * STOP 或 SUPERSEDE 的 START 可能尚未上网，但它们携带撤回历史的意图。
   * 连续切代时保留这些标记的顺序，只清掉旧普通 PCM；否则紧接着的普通问题
   * 可能绕过前一次撤回，让服务器把用户没听完的回答保留在历史中。
   */
  void AdvanceLocked(std::uint32_t generation, bool active) {
    generation_ = generation;
    active_ = active;
    uplink_sequence_ = 0;
    downlink_sequence_ = 0;
    input_ended_ = false;
    audio_ended_ = false;
    reply_done_ = false;
    outbound_.erase(std::remove_if(outbound_.begin(), outbound_.end(),
                                   [](const Outbound& frame) {
                                     return !frame.retirement;
                                   }),
                    outbound_.end());
    events_.erase(std::remove_if(events_.begin(), events_.end(),
                                 [](const LinkEvent& event) {
                                   return event.generation != 0;
                                 }),
                  events_.end());
  }

  void FailLocked(const char* code) {
    if (failed_) {
      return;
    }
    failed_ = true;
    online_ = false;
    outbound_.clear();
    events_.clear();
    // 只报告固定阶段码，不把原始 TLS/provider 消息或凭据带到应用日志。
    LinkEvent event;
    event.kind = LinkEventKind::Offline;
    event.code = code;
    events_.push_back(std::move(event));
    changed_.notify_all();
  }

  void Fail(const char* code) {
    std::lock_guard<std::mutex> lock(mutex_);
    FailLocked(code);
  }

  /// @brief 旧代直接忽略；当前代必须按上传结束、音频连续、END、done 的顺序推进。
  bool AcceptReplyLocked(const LinkEvent& event) {
    if (!online_) {
      FailLocked("message_before_ready");
      return false;
    }
    if (event.generation < generation_) {
      return false;
    }
    if (event.generation > generation_ || generation_ == 0) {
      FailLocked("future_generation");
      return false;
    }
    if (!active_) {
      return false;  // STOP 的 generation 只用于退休，不能产生回答。
    }
    if (reply_done_ || (!input_ended_ && event.kind != LinkEventKind::Error)) {
      FailLocked("reply_order");
      return false;
    }

    if (event.kind == LinkEventKind::Audio) {
      if (audio_ended_ || event.sequence != downlink_sequence_) {
        FailLocked("audio_sequence");
        return false;
      }
      ++downlink_sequence_;
      audio_ended_ = event.end;
    } else if (event.kind == LinkEventKind::Done) {
      if (downlink_sequence_ != 0 && !audio_ended_) {
        FailLocked("done_before_audio_end");
        return false;
      }
      reply_done_ = true;
    } else if (event.kind == LinkEventKind::Error) {
      reply_done_ = true;
    }
    return true;
  }

  void OnEvent(LinkEvent event) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (failed_ || stop_.load()) {
      return;
    }
    if (event.kind == LinkEventKind::Online) {
      if (online_) {
        FailLocked("duplicate_ready");
        return;
      }
      online_ = true;
    } else if (!AcceptReplyLocked(event)) {
      return;
    }
    if (events_.size() == kEventCapacity) {
      FailLocked("inbound_overflow");
      return;
    }
    events_.push_back(std::move(event));
  }

  /**
   * @brief 用保存的 SPKI 摘要验证课堂自签证书，并检查证书的服务器用途。
   *
   * 设备以公钥 pin 为信任锚，不依赖公网 CA。失败必须终止握手；
   * OpenSSL 通过 C ABI 调用这里，任何 C++ 异常都必须在回调内部收住。
   */
  static int VerifyPin(X509_STORE_CTX* store, void* argument) {
    try {
      auto* self = static_cast<Impl*>(argument);
      X509* certificate = X509_STORE_CTX_get0_cert(store);
      if (self == nullptr || certificate == nullptr ||
          X509_check_purpose(certificate, X509_PURPOSE_SSL_SERVER, 0) <= 0) {
        return 0;
      }
      X509_PUBKEY* key = X509_get_X509_PUBKEY(certificate);
      const int encoded_size = i2d_X509_PUBKEY(key, nullptr);
      if (encoded_size <= 0) {
        return 0;
      }
      std::vector<unsigned char> encoded_key(static_cast<std::size_t>(encoded_size));
      unsigned char* cursor = encoded_key.data();
      std::array<unsigned char, 32> digest{};
      unsigned int digest_size = 0;
      if (i2d_X509_PUBKEY(key, &cursor) != encoded_size ||
          EVP_Digest(encoded_key.data(), encoded_key.size(), digest.data(), &digest_size,
                     EVP_sha256(), nullptr) != 1 ||
          digest_size != digest.size() ||
          CRYPTO_memcmp(digest.data(), self->pin_.data(), digest.size()) != 0) {
        return 0;
      }
      X509_STORE_CTX_set_error(store, X509_V_OK);
      return 1;
    } catch (...) {
      return 0;
    }
  }

  websocketpp::lib::shared_ptr<TlsContext> MakeTls() {
    auto context = websocketpp::lib::make_shared<TlsContext>(TlsContext::tls_client);
    SSL_CTX_set_min_proto_version(context->native_handle(), TLS1_2_VERSION);
    SSL_CTX_set_options(context->native_handle(), SSL_OP_NO_COMPRESSION);
    SSL_CTX_set_verify(context->native_handle(), SSL_VERIFY_PEER, nullptr);
    SSL_CTX_set_cert_verify_callback(context->native_handle(), &VerifyPin, this);
    return context;
  }

  void SendHello(Client& client, Hdl handle) {
    // socket_init 时 TCP 还未打开。握手后再设置小发送缓冲和 TCP_NODELAY，
    // 避免内核排队数秒旧 PCM，也让短控制帧及时发出。
    websocketpp::lib::asio::error_code socket_error;
    auto& socket = client.get_con_from_hdl(handle)->get_raw_socket();
    socket.set_option(websocketpp::lib::asio::socket_base::send_buffer_size(4096),
                      socket_error);
    if (!socket_error) {
      socket.set_option(websocketpp::lib::asio::ip::tcp::no_delay(true), socket_error);
    }
    if (socket_error) {
      Fail("socket_setup");
      return;
    }
    const std::string hello = "{\"type\":\"hello\",\"device_id\":\"" + configured_.device_id +
                              "\",\"token\":\"" + kTeachingToken + "\"}";
    websocketpp::lib::error_code error;
    client.send(handle, hello, websocketpp::frame::opcode::text, error);
    if (error) {
      Fail("hello_send");
    }
  }

  void OnMessage(Client::message_ptr message) {
    try {
      if (message->get_opcode() == websocketpp::frame::opcode::text) {
        OnEvent(detail::DecodeText(message->get_payload()));
      } else if (message->get_opcode() == websocketpp::frame::opcode::binary) {
        OnEvent(detail::DecodeAudio(message->get_payload()));
      } else {
        Fail("websocket_opcode");
      }
    } catch (...) {
      Fail("invalid_protocol");
    }
  }

  void Configure(Client& client) {
    client.clear_access_channels(websocketpp::log::alevel::all);
    client.clear_error_channels(websocketpp::log::elevel::all);
    client.init_asio();
    client.start_perpetual();
    client.set_tls_init_handler([this](Hdl) {
      return MakeTls();
    });
    client.set_open_handler([this, &client](Hdl handle) {
      SendHello(client, handle);
    });
    client.set_fail_handler([this](Hdl) {
      Fail("tls_connect");
    });
    client.set_close_handler([this](Hdl) {
      Fail("connection_closed");
    });
    client.set_pong_handler([this](Hdl, const std::string& payload) {
      if (payload == "boompi") {
        awaiting_pong_ = false;
      }
    });
    client.set_message_handler([this](Hdl, Client::message_ptr message) {
      OnMessage(message);
    });
  }

  void SendNextFrame(Client::connection_ptr connection) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!online_ || failed_) {
      return;
    }
    const auto now = Clock::now();
    wire_bytes_ = connection->get_buffered_amount();
    // 一次最多交出一帧；未交出的帧留在自有队列，切代时仍能及时退休。
    if (wire_bytes_ != 0) {
      if (now - wire_started_ > std::chrono::milliseconds(800)) {
        FailLocked("send_timeout");
      }
      return;
    }
    if (outbound_.empty()) {
      return;
    }
    const Outbound& frame = outbound_.front();
    if (now - frame.queued_at > std::chrono::milliseconds(800)) {
      FailLocked("uplink_timeout");
      return;
    }
    auto opcode = websocketpp::frame::opcode::binary;
    if (frame.text) {
      opcode = websocketpp::frame::opcode::text;
    }
    const auto error = connection->send(frame.bytes.data(), frame.size, opcode);
    if (error) {
      FailLocked("send_failed");
      return;
    }
    wire_bytes_ = frame.size;
    wire_started_ = now;
    outbound_.pop_front();
  }

  bool RunConnection(Client& client, Client::connection_ptr connection) {
    const auto started = Clock::now();
    auto ping_at = started;
    awaiting_pong_ = false;
    bool reached_ready = false;
    while (!stop_.load()) {
      client.poll();
      SendNextFrame(connection);
      std::unique_lock<std::mutex> lock(mutex_);
      if (failed_) {
        break;
      }
      const auto now = Clock::now();
      reached_ready = reached_ready || online_;
      if (!online_ && now - started > std::chrono::seconds(5)) {
        FailLocked("hello_timeout");
        break;
      }
      if (awaiting_pong_ && now - ping_at > std::chrono::seconds(5)) {
        FailLocked("heartbeat_timeout");
        break;
      }
      if (online_ && !awaiting_pong_ && now - ping_at >= std::chrono::seconds(5)) {
        websocketpp::lib::error_code error;
        connection->ping("boompi", error);
        if (error) {
          FailLocked("heartbeat_send");
          break;
        }
        awaiting_pong_ = true;
        ping_at = now;
      }
      changed_.wait_for(lock, std::chrono::milliseconds(2));
    }
    return reached_ready;
  }

  bool Connect(const LinkConfig& endpoint) {
    std::array<unsigned char, 33> pin{};
    if (!config::IsValidSpkiSha256(endpoint.spki) ||
        EVP_DecodeBlock(pin.data(),
                        reinterpret_cast<const unsigned char*>(endpoint.spki.data()),
                        44) != 33) {
      Fail("invalid_pin");
      return false;
    }
    std::copy_n(pin.begin(), pin_.size(), pin_.begin());

    Client client;
    Configure(client);
    websocketpp::lib::error_code error;
    const std::string address =
        "wss://" + endpoint.host + ":" + std::to_string(endpoint.port) + "/ws";
    const auto connection = client.get_connection(address, error);
    if (error) {
      Fail("invalid_endpoint");
      return false;
    }
    connection->set_open_handshake_timeout(5000);
    connection->set_max_message_size(8192);
    client.connect(connection);
    const bool reached_ready = RunConnection(client, connection);

    // 直接取消当前 socket，退出不等待远端完成 close 握手或 TCP/TLS 超时。
    websocketpp::lib::asio::error_code socket_error;
    connection->get_raw_socket().cancel(socket_error);
    connection->get_raw_socket().close(socket_error);
    client.stop_perpetual();
    client.stop();
    return reached_ready;
  }

  void Run() {
    auto backoff = std::chrono::milliseconds(500);
    while (!stop_.load()) {
      {
        std::lock_guard<std::mutex> lock(mutex_);
        failed_ = false;
        online_ = false;
        wire_bytes_ = 0;
        outbound_.clear();
        AdvanceLocked(0, false);
      }
      bool connected = false;
      try {
        LinkConfig endpoint;
        if (!detail::FindServer(configured_, &endpoint, &stop_)) {
          Fail("network_setup");
        } else if (!stop_.load()) {
          connected = Connect(endpoint);
        }
      } catch (...) {
        Fail("network_worker");
      }
      if (connected) {
        backoff = std::chrono::milliseconds(500);
      }
      std::unique_lock<std::mutex> lock(mutex_);
      changed_.wait_for(lock, backoff, [this] {
        return stop_.load();
      });
      backoff = std::min(backoff * 2, std::chrono::milliseconds(8000));
    }
  }

  LinkConfig configured_;
  std::thread thread_;
  std::atomic<bool> stop_{false};
  std::mutex mutex_;
  std::condition_variable changed_;
  std::deque<Outbound> outbound_;
  std::deque<LinkEvent> events_;
  std::array<unsigned char, 32> pin_{};
  std::size_t wire_bytes_{0};
  Clock::time_point wire_started_{Clock::now()};
  std::uint32_t generation_{0};
  std::uint32_t uplink_sequence_{0};
  std::uint32_t downlink_sequence_{0};
  bool online_{false};
  bool failed_{false};
  bool active_{false};
  bool input_ended_{false};
  bool audio_ended_{false};
  bool reply_done_{false};
  bool awaiting_pong_{false};  // 只在网络线程访问；其余协议字段由 mutex 保护。
};

VoiceLink::VoiceLink() : impl_(std::make_unique<Impl>()) {}
VoiceLink::~VoiceLink() = default;

bool VoiceLink::Open(const LinkConfig& settings) {
  return impl_->Open(settings);
}

bool VoiceLink::Poll(LinkEvent* event) {
  return impl_->Poll(event);
}

SendResult VoiceLink::SendAudio(std::uint32_t generation, const std::int16_t* pcm, bool start,
                                bool end, bool supersede) {
  return impl_->SendAudio(generation, pcm, start, end, supersede);
}

bool VoiceLink::Stop(std::uint32_t generation, bool retract) {
  return impl_->Stop(generation, retract);
}

void VoiceLink::Close() noexcept {
  impl_->Close();
}

}  // namespace boompi::network
