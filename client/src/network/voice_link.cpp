#include "boompi/network/voice_link.h"

#include "boompi/config/voice_client_config.h"
#include "voice_codec.h"

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

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>
#include <websocketpp/client.hpp>
#include <websocketpp/config/asio_client.hpp>

namespace boompi::network {

class VoiceLink::Impl final {
  using Clock = std::chrono::steady_clock;
  using Client = websocketpp::client<websocketpp::config::asio_tls_client>;
  using Hdl = websocketpp::connection_hdl;
  using TlsContext = websocketpp::lib::asio::ssl::context;
  static constexpr std::size_t kQueueFrames = 800 / 20;
  // WebSocket++的buffered_amount不含正在async_write的帧；另预留两个位置，
  // 覆盖一个正在写的帧和一个已进入WebSocket++发送队列的帧。
  static constexpr std::size_t kTransportFrames = 2;
  static constexpr std::size_t kEventCapacity = 64;
  struct Outbound final {
    std::array<std::uint8_t, detail::kFrameBytes> bytes{};
    std::size_t size{0};
    bool text{false};
    bool retirement{false};
    Clock::time_point queued_at{Clock::now()};
  };

 public:
  ~Impl() { Close(); }

  bool Open(const LinkConfig& config) {
    if (thread_.joinable() || !config::IsValidDeviceId(config.device_id)) return false;
    if (!config.host.empty()) {
      websocketpp::lib::asio::error_code ec;
      const auto address = websocketpp::lib::asio::ip::address::from_string(config.host, ec);
      if (ec || !address.is_v4() || !config.port || !config::IsValidSpkiSha256(config.spki)) return false;
    } else if (!config.spki.empty()) return false;
    configured_ = config;
    stop_.store(false);
    try { thread_ = std::thread(&Impl::Run, this); }
    catch (...) { return false; }
    return true;
  }

  bool Poll(LinkEvent* event) {
    if (!event) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    if (events_.empty()) return false;
    *event = std::move(events_.front());
    events_.pop_front();
    return true;
  }

  SendResult SendAudio(std::uint32_t generation, const std::int16_t* pcm,
                       bool start, bool end, bool supersede) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!online_ || failed_ || stop_.load()) return SendResult::Disconnected;
    if (!pcm || !generation || (supersede && !start) ||
        (start ? generation <= generation_ : generation != generation_ || !active_ || input_ended_) ||
        (!start && uplink_sequence_ == UINT32_MAX)) {
      FailLocked("invalid_uplink");
      return SendResult::Disconnected;
    }
    // 普通帧与正在写的帧合计最多800ms；紧急切代清除旧待发帧，另有一个保留槽。
    if (!supersede && outbound_.size() + kTransportFrames >= kQueueFrames)
      return SendResult::Backpressure;
    if (start) AdvanceLocked(generation, true);
    if (outbound_.size() + kTransportFrames >= kQueueFrames) {
      FailLocked("retirement_overflow"); return SendResult::Disconnected;
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
    if (!online_ || failed_ || stop_.load()) return false;
    if (!generation || generation <= generation_) { FailLocked("invalid_stop"); return false; }
    AdvanceLocked(generation, false);
    if (outbound_.size() >= kQueueFrames) { FailLocked("retirement_overflow"); return false; }
    const std::string text = "{\"type\":\"stop\",\"generation\":" + std::to_string(generation) +
                             ",\"retract\":" + (retract ? "true}" : "false}");
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
    if (thread_.joinable()) thread_.join();
    std::lock_guard<std::mutex> lock(mutex_);
    online_ = false;
    events_.clear();
    outbound_.clear();
  }

 private:
  // 调用线程和网络线程共享这把锁：切代、过滤旧事件与提交START形成同一个边界。
  void AdvanceLocked(std::uint32_t generation, bool active) {
    generation_ = generation;
    active_ = active;
    uplink_sequence_ = downlink_sequence_ = 0;
    input_ended_ = audio_ended_ = reply_done_ = false;
    // 已接受的STOP/打断START可能尚未写到socket，保留其撤回历史语义，
    // 新代必须排在这些标记之后；只删除可退休的旧PCM，不能删除撤回动作。
    outbound_.erase(std::remove_if(outbound_.begin(), outbound_.end(),
        [](const Outbound& frame) { return !frame.retirement; }), outbound_.end());
    events_.erase(std::remove_if(events_.begin(), events_.end(),
        [](const LinkEvent& event) { return event.generation != 0; }), events_.end());
  }

  void FailLocked(const char* code) {
    if (failed_) return;
    failed_ = true;
    online_ = false;
    outbound_.clear();
    events_.clear();
    // 网络错误携带阶段码，避免原始TLS/provider内容或凭据进入应用日志。
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

  void OnEvent(LinkEvent event) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (failed_ || stop_.load()) return;
    if (event.kind == LinkEventKind::Online) {
      if (online_) { FailLocked("duplicate_ready"); return; }
      online_ = true;
    } else {
      if (!online_) { FailLocked("message_before_ready"); return; }
      if (event.generation < generation_) return;
      if (event.generation > generation_ || !generation_) { FailLocked("future_generation"); return; }
      if (!active_) return;  // STOP建立的generation不是一个可接收回答的轮次。
      if (reply_done_ || (!input_ended_ && event.kind != LinkEventKind::Error)) {
        FailLocked("reply_order"); return;
      }
      if (event.kind == LinkEventKind::Audio) {
        if (audio_ended_ || event.sequence != downlink_sequence_) { FailLocked("audio_sequence"); return; }
        ++downlink_sequence_;
        audio_ended_ = event.end;
      } else if (event.kind == LinkEventKind::Done) {
        if (downlink_sequence_ != 0 && !audio_ended_) { FailLocked("done_before_audio_end"); return; }
        reply_done_ = true;
      } else if (event.kind == LinkEventKind::Error) {
        reply_done_ = true;
      }
    }
    if (events_.size() == kEventCapacity) { FailLocked("inbound_overflow"); return; }
    events_.push_back(std::move(event));
  }

  static int VerifyPin(X509_STORE_CTX* store, void* argument) {
    try {
    // 课堂自签证书以已保存SPKI为信任锚，校验用途；不提供跳过验证的分支。
    auto* self = static_cast<Impl*>(argument);
    X509* cert = X509_STORE_CTX_get0_cert(store);
    if (!self || !cert || X509_check_purpose(cert, X509_PURPOSE_SSL_SERVER, 0) <= 0) return 0;
    X509_PUBKEY* key = X509_get_X509_PUBKEY(cert);
    const int count = i2d_X509_PUBKEY(key, nullptr);
    if (count <= 0) return 0;
    std::vector<unsigned char> der(static_cast<std::size_t>(count));
    unsigned char* cursor = der.data();
    std::array<unsigned char, 32> digest{};
    unsigned int size = 0;
    if (i2d_X509_PUBKEY(key, &cursor) != count ||
        EVP_Digest(der.data(), der.size(), digest.data(), &size, EVP_sha256(), nullptr) != 1 ||
        size != digest.size() || CRYPTO_memcmp(digest.data(), self->pin_.data(), digest.size()) != 0) return 0;
    X509_STORE_CTX_set_error(store, X509_V_OK);
    return 1;
    } catch (...) {
      // OpenSSL通过C ABI回调；分配失败必须成为握手失败，异常不能越过C栈帧。
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

  void Configure(Client& client) {
    client.clear_access_channels(websocketpp::log::alevel::all);
    client.clear_error_channels(websocketpp::log::elevel::all);
    client.init_asio();
    client.start_perpetual();
    client.set_tls_init_handler([this](Hdl) { return MakeTls(); });
    client.set_open_handler([this, &client](Hdl handle) {
      // socket_init时TCP尚未open，此时设置才会生效；避免内核隐藏数秒旧PCM。
      websocketpp::lib::asio::error_code socket_error;
      auto& socket = client.get_con_from_hdl(handle)->get_raw_socket();
      socket.set_option(
          websocketpp::lib::asio::socket_base::send_buffer_size(4096), socket_error);
      if (!socket_error) socket.set_option(websocketpp::lib::asio::ip::tcp::no_delay(true), socket_error);
      if (socket_error) { Fail("socket_setup"); return; }
      const std::string hello = "{\"type\":\"hello\",\"device_id\":\"" + configured_.device_id +
                                "\",\"token\":\"" + detail::kTeachingToken + "\"}";
      websocketpp::lib::error_code ec;
      client.send(handle, hello, websocketpp::frame::opcode::text, ec);
      if (ec) Fail("hello_send");
    });
    client.set_fail_handler([this](Hdl) { Fail("tls_connect"); });
    client.set_close_handler([this](Hdl) { Fail("connection_closed"); });
    client.set_pong_handler([this](Hdl, const std::string& payload) {
      if (payload == "boompi") awaiting_pong_ = false;
    });
    client.set_message_handler([this](Hdl, Client::message_ptr message) {
      try {
        if (message->get_opcode() == websocketpp::frame::opcode::text)
          OnEvent(detail::DecodeText(message->get_payload()));
        else if (message->get_opcode() == websocketpp::frame::opcode::binary)
          OnEvent(detail::DecodeAudio(message->get_payload()));
        else Fail("websocket_opcode");
      } catch (...) { Fail("invalid_protocol"); }
    });
  }

  void Pump(Client::connection_ptr connection) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!online_ || failed_) return;
    const auto now = Clock::now();
    wire_bytes_ = connection->get_buffered_amount();
    // WebSocket++最多一个正在写的帧加一个排队帧；其余旧代仍在自有队列可清理。
    if (wire_bytes_) {
      if (now - wire_started_ > std::chrono::milliseconds(800)) FailLocked("send_timeout");
      return;
    }
    if (outbound_.empty()) return;
    const auto& frame = outbound_.front();
    if (now - frame.queued_at > std::chrono::milliseconds(800)) { FailLocked("uplink_timeout"); return; }
    const auto ec = connection->send(frame.bytes.data(), frame.size,
        frame.text ? websocketpp::frame::opcode::text : websocketpp::frame::opcode::binary);
    if (ec) { FailLocked("send_failed"); return; }
    wire_bytes_ = frame.size;
    wire_started_ = now;
    outbound_.pop_front();
  }

  bool Connect(const LinkConfig& endpoint) {
    std::array<unsigned char, 33> pin{};
    if (!config::IsValidSpkiSha256(endpoint.spki) || EVP_DecodeBlock(pin.data(),
        reinterpret_cast<const unsigned char*>(endpoint.spki.data()), 44) != 33) {
      Fail("invalid_pin"); return false;
    }
    std::copy_n(pin.begin(), pin_.size(), pin_.begin());
    Client client;
    Configure(client);
    websocketpp::lib::error_code ec;
    const auto connection = client.get_connection("wss://" + endpoint.host + ":" +
        std::to_string(endpoint.port) + "/ws", ec);
    if (ec) { Fail("invalid_endpoint"); return false; }
    connection->set_open_handshake_timeout(5000);
    connection->set_max_message_size(8192);
    client.connect(connection);
    const auto started = Clock::now();
    auto ping_at = started;
    awaiting_pong_ = false;
    bool reached_ready = false;
    while (!stop_.load()) {
      client.poll();
      Pump(connection);
      std::unique_lock<std::mutex> lock(mutex_);
      if (failed_) break;
      const auto now = Clock::now();
      reached_ready = reached_ready || online_;
      if (!online_ && now - started > std::chrono::seconds(5)) { FailLocked("hello_timeout"); break; }
      if (awaiting_pong_ && now - ping_at > std::chrono::seconds(5)) { FailLocked("heartbeat_timeout"); break; }
      if (online_ && !awaiting_pong_ && now - ping_at >= std::chrono::seconds(5)) {
        connection->ping("boompi", ec);
        if (ec) { FailLocked("heartbeat_send"); break; }
        awaiting_pong_ = true;
        ping_at = now;
      }
      changed_.wait_for(lock, std::chrono::milliseconds(2));
    }
    // 直接关闭当前socket，取消TCP/TLS中的等待；退出不依赖远端close握手。
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
        failed_ = online_ = false;
        wire_bytes_ = 0;
        outbound_.clear();
        AdvanceLocked(0, false);
      }
      bool connected = false;
      try {
        LinkConfig endpoint;
        if (!detail::FindServer(configured_, &endpoint, &stop_)) Fail("network_setup");
        else if (!stop_.load()) connected = Connect(endpoint);
      } catch (...) { Fail("network_worker"); }
      if (connected) backoff = std::chrono::milliseconds(500);
      std::unique_lock<std::mutex> lock(mutex_);
      changed_.wait_for(lock, backoff, [this] { return stop_.load(); });
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
  std::uint32_t generation_{0}, uplink_sequence_{0}, downlink_sequence_{0};
  bool online_{false}, failed_{false}, active_{false};
  bool input_ended_{false}, audio_ended_{false}, reply_done_{false};
  bool awaiting_pong_{false};  // 仅网络线程访问；其余协议字段由mutex保护。
};

VoiceLink::VoiceLink() : impl_(std::make_unique<Impl>()) {}
VoiceLink::~VoiceLink() = default;
bool VoiceLink::Open(const LinkConfig& config) { return impl_->Open(config); }
bool VoiceLink::Poll(LinkEvent* event) { return impl_->Poll(event); }
SendResult VoiceLink::SendAudio(std::uint32_t generation, const std::int16_t* pcm,
                                bool start, bool end, bool supersede) {
  return impl_->SendAudio(generation, pcm, start, end, supersede);
}
bool VoiceLink::Stop(std::uint32_t generation, bool retract) { return impl_->Stop(generation, retract); }
void VoiceLink::Close() noexcept { impl_->Close(); }

}  // namespace boompi::network
