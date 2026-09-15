#include "boompi/network/voice_net.h"

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
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

namespace boompi::voice_net {
namespace {

using Clock = std::chrono::steady_clock;
using Client = websocketpp::client<websocketpp::config::asio_tls_client>;
using Hdl = websocketpp::connection_hdl;
using TlsContext = websocketpp::lib::asio::ssl::context;

// 只跟踪本轮协议进度；DONE 不代表声卡已播完，尾播由音频线程判断。
enum class TurnPhase { Retired, Uploading, WaitingReply, Complete };
enum class ConnectionState { Offline, Online, Failed };

// 按音频时长预算排队容量，最多容纳约 800 ms；更久应明确失败，不能累积旧语音。
static constexpr std::size_t kQueueFrames = 800 / 20;
// buffered_amount 不包含正在 async_write 的帧。固定预留两帧容量，覆盖正在写的
// 一帧和已交给 WebSocket++ 排队的一帧，不能把这部分误算为“队列已空”。
static constexpr std::size_t kTransportFrames = 2;
// 接收队列还包含字幕和控制事件，不只计 PCM；溢出时整条连接失败以免掩盖缺帧。
static constexpr std::size_t kEventCapacity = 64;
// 固定课堂 hello 口令，与只保存在服务端的 DashScope Key 无关。
static constexpr char kTeachingToken[] = "boompi-teaching-shared-token-v1-2026";

/**
 * @brief 已拥有线协议字节的待发帧；普通 PCM 与 CANCEL 共用同一个发送顺序。
 *
 * retirement 标记携带“退休旧轮次”副作用的 CANCEL 或 START(supersede)，
 * 后续切代不能像普通 PCM 一样将其删除；queued_at 用于限制滞留时间。
 */
struct Outbound final {
  std::array<std::uint8_t, detail::kFrameBytes> bytes{};
  std::size_t size{0};
  bool text{false};
  bool retirement{false};
  Clock::time_point queued_at{Clock::now()};
};

// Open 在线程启动前写入，此后只读；thread_ 的创建、join 由应用生命周期串行控制。
config::VoiceClientConfig configured_;
std::thread thread_;
std::atomic<bool> stop_{false};
// 除 stop_ 外，跨线程收发队列、连接状态和以下轮次字段都在 mutex_ 下访问。
std::mutex mutex_;
std::condition_variable changed_;
std::deque<Outbound> outbound_;
std::deque<LinkEvent> events_;
// TLS pin 与底层发送计时只在网络线程访问，不需要应用线程读取。
std::array<unsigned char, 32> pin_{};
Clock::time_point wire_started_{Clock::now()};
// generation 由 actor 分配，网络只记录当前值并独立跟踪该代上行/下行的 sequence。
std::uint32_t generation_{0};
std::uint32_t uplink_sequence_{0};
std::uint32_t downlink_sequence_{0};
ConnectionState connection_state_{ConnectionState::Offline};
TurnPhase turn_phase_{TurnPhase::Retired};
bool audio_ended_{false};
bool awaiting_pong_{false};  // 只在网络线程访问；其余协议字段由 mutex 保护。
void AdvanceLocked(std::uint32_t generation, TurnPhase phase);
void FailLocked(const char* code);
void Fail(const char* code);
bool AcceptReplyLocked(const LinkEvent& event);
void OnEvent(LinkEvent event);
int VerifyPin(X509_STORE_CTX* store, void* argument);
websocketpp::lib::shared_ptr<TlsContext> MakeTls();
void SendHello(Client& client, Hdl handle);
void OnMessage(Client::message_ptr message);
void Configure(Client& client);
void SendNextFrame(Client::connection_ptr connection);
bool ProcessConnection(Client& client, Client::connection_ptr connection);
bool Connect(const config::VoiceClientConfig& endpoint);
void NetworkTask();
void AdvanceLocked(std::uint32_t generation, TurnPhase phase) {
  generation_ = generation;
  turn_phase_ = phase;
  uplink_sequence_ = 0;
  downlink_sequence_ = 0;
  audio_ended_ = false;
  outbound_.erase(std::remove_if(outbound_.begin(), outbound_.end(),
                                 [](const Outbound& frame) {
                                   return !frame.retirement;
                                 }),
                  outbound_.end());
  // 保留 generation 0 的连接状态通知；所有回答事件都属于已退休的业务代。
  events_.erase(std::remove_if(events_.begin(), events_.end(),
                               [](const LinkEvent& event) {
                                 return event.generation != 0;
                               }),
                events_.end());
}

void FailLocked(const char* code) {
  if (connection_state_ == ConnectionState::Failed) {
    return;
  }
  connection_state_ = ConnectionState::Failed;
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

bool AcceptReplyLocked(const LinkEvent& event) {
  if (connection_state_ != ConnectionState::Online) {
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
  if (turn_phase_ == TurnPhase::Retired) {
    return false;  // CANCEL 的 generation 只用于退休，不能产生回答。
  }
  if (turn_phase_ == TurnPhase::Complete ||
      (turn_phase_ == TurnPhase::Uploading && event.kind != LinkEventKind::Error)) {
    FailLocked("reply_order");
    return false;
  }

  if (event.kind == LinkEventKind::Audio) {
    if (audio_ended_ || event.sequence != downlink_sequence_) {
      FailLocked("audio_sequence");
      return false;
    }
    ++downlink_sequence_;
    audio_ended_ = event.audio_size < detail::kPcmBytes;
  } else if (event.kind == LinkEventKind::Done) {
    turn_phase_ = TurnPhase::Complete;
  } else if (event.kind == LinkEventKind::Error) {
    turn_phase_ = TurnPhase::Complete;
  }
  return true;
}

void OnEvent(LinkEvent event) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (connection_state_ == ConnectionState::Failed || stop_.load()) {
    return;
  }
  if (event.kind == LinkEventKind::Online) {
    if (connection_state_ == ConnectionState::Online) {
      FailLocked("duplicate_ready");
      return;
    }
    connection_state_ = ConnectionState::Online;
  } else if (!AcceptReplyLocked(event)) {
    return;
  }
  if (events_.size() == kEventCapacity) {
    FailLocked("inbound_overflow");
    return;
  }
  events_.push_back(std::move(event));
}

int VerifyPin(X509_STORE_CTX* store, void*) {
  try {
    X509* certificate = X509_STORE_CTX_get0_cert(store);
    if (certificate == nullptr ||
        X509_check_purpose(certificate, X509_PURPOSE_SSL_SERVER, 0) <= 0) {
      return 0;
    }
    X509_PUBKEY* key = X509_get_X509_PUBKEY(certificate);
    // pin 覆盖 DER 编码的 SubjectPublicKeyInfo，不是整张证书或 PEM 文本的摘要。
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
        CRYPTO_memcmp(digest.data(), pin_.data(), digest.size()) != 0) {
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
  SSL_CTX_set_cert_verify_callback(context->native_handle(), &VerifyPin, nullptr);
  return context;
}

void SendHello(Client& client, Hdl handle) {
  // socket_init 时 TCP 还未打开。握手后再设置小发送缓冲和 TCP_NODELAY，
  // 避免内核排队数秒旧 PCM，也让短控制帧及时发出。
  websocketpp::lib::asio::error_code socket_error;
  auto& socket = client.get_con_from_hdl(handle)->get_raw_socket();
  socket.set_option(websocketpp::lib::asio::socket_base::send_buffer_size(4096), socket_error);
  if (!socket_error) {
    socket.set_option(websocketpp::lib::asio::ip::tcp::no_delay(true), socket_error);
  }
  if (socket_error) {
    Fail("socket_setup");
    return;
  }
  const std::string hello = "{\"type\":\"hello\",\"device_id\":\"" + configured_.device_id +
                            "\",\"token\":\"" + kTeachingToken +
                            "\",\"sample_rate\":16000,\"version\":3}";
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
  client.set_tls_init_handler([](Hdl) {
    return MakeTls();
  });
  client.set_open_handler([&client](Hdl handle) {
    SendHello(client, handle);
  });
  client.set_fail_handler([](Hdl) {
    Fail("tls_connect");
  });
  client.set_close_handler([](Hdl) {
    Fail("connection_closed");
  });
  client.set_pong_handler([](Hdl, const std::string& payload) {
    if (payload == "boompi") {
      awaiting_pong_ = false;
    }
  });
  client.set_message_handler([](Hdl, Client::message_ptr message) {
    OnMessage(message);
  });
}

void SendNextFrame(Client::connection_ptr connection) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (connection_state_ != ConnectionState::Online) {
    return;
  }
  const auto now = Clock::now();
  // 一次最多交出一帧；未交出的帧留在自有队列，切代时仍能及时退休。
  if (connection->get_buffered_amount() != 0) {
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
  wire_started_ = now;
  outbound_.pop_front();
}

bool ProcessConnection(Client& client, Client::connection_ptr connection) {
  const auto started = Clock::now();
  auto ping_at = started;
  awaiting_pong_ = false;
  bool reached_ready = false;
  while (!stop_.load()) {
    // 1. 处理已到达的服务器消息，再发送应用排队的录音或 CANCEL。
    client.poll();
    SendNextFrame(connection);
    std::unique_lock<std::mutex> lock(mutex_);
    if (connection_state_ == ConnectionState::Failed) {
      break;
    }
    // 2. 检查握手与心跳；断线交给外层网络任务重连。
    const auto now = Clock::now();
    reached_ready = reached_ready || connection_state_ == ConnectionState::Online;
    if (connection_state_ != ConnectionState::Online &&
        now - started > std::chrono::seconds(5)) {
      FailLocked("hello_timeout");
      break;
    }
    if (awaiting_pong_ && now - ping_at > std::chrono::seconds(5)) {
      FailLocked("heartbeat_timeout");
      break;
    }
    if (connection_state_ == ConnectionState::Online && !awaiting_pong_ &&
        now - ping_at >= std::chrono::seconds(5)) {
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

bool Connect(const config::VoiceClientConfig& endpoint) {
  std::array<unsigned char, 33> pin{};
  if (!config::IsValidSpkiSha256(endpoint.server_spki_sha256) ||
      EVP_DecodeBlock(
          pin.data(),
          reinterpret_cast<const unsigned char*>(endpoint.server_spki_sha256.data()),
          44) != 33) {
    Fail("invalid_pin");
    return false;
  }
  std::copy_n(pin.begin(), pin_.size(), pin_.begin());

  Client client;
  Configure(client);
  websocketpp::lib::error_code error;
  const std::string address =
      "wss://" + endpoint.server_ip + ":" + std::to_string(endpoint.server_port) + "/ws";
  const auto connection = client.get_connection(address, error);
  if (error) {
    Fail("invalid_endpoint");
    return false;
  }
  connection->set_open_handshake_timeout(5000);
  connection->set_max_message_size(8192);
  client.connect(connection);
  const bool reached_ready = ProcessConnection(client, connection);

  // 直接取消当前 socket，退出不等待远端完成 close 握手或 TCP/TLS 超时。
  websocketpp::lib::asio::error_code socket_error;
  connection->get_raw_socket().cancel(socket_error);
  connection->get_raw_socket().close(socket_error);
  client.stop_perpetual();
  client.stop();
  return reached_ready;
}

void NetworkTask() {
  auto backoff = std::chrono::milliseconds(500);
  while (!stop_.load()) {
    // 1. 清理上一条连接的数据，旧问题不会在重连后重新发送。
    {
      std::lock_guard<std::mutex> lock(mutex_);
      connection_state_ = ConnectionState::Offline;
      outbound_.clear();
      AdvanceLocked(0, TurnPhase::Retired);
    }
    bool connected = false;
    try {
      // 2. 准备网卡、确定地址，然后持续处理这条 WSS 连接。
      config::VoiceClientConfig endpoint;
      if (!detail::FindServer(configured_, &endpoint, &stop_)) {
        Fail("network_setup");
      } else if (!stop_.load()) {
        connected = Connect(endpoint);
      }
    } catch (...) {
      Fail("network_worker");
    }
    // 3. 连接结束后稍等再试；关闭请求可以提前唤醒等待。
    if (connected) {
      backoff = std::chrono::milliseconds(500);
    }
    std::unique_lock<std::mutex> lock(mutex_);
    changed_.wait_for(lock, backoff, [] {
      return stop_.load();
    });
    backoff = std::min(backoff * 2, std::chrono::milliseconds(8000));
  }
}
}  // namespace

bool open(const config::VoiceClientConfig& settings) {
  if (thread_.joinable() || !config::IsValidDeviceId(settings.device_id)) {
    return false;
  }
  if (!settings.server_ip.empty()) {
    websocketpp::lib::asio::error_code error;
    const auto address =
        websocketpp::lib::asio::ip::address::from_string(settings.server_ip, error);
    if (error || !address.is_v4() || settings.server_port == 0 ||
        !config::IsValidSpkiSha256(settings.server_spki_sha256)) {
      return false;
    }
  } else if (!settings.server_spki_sha256.empty()) {
    return false;
  }
  configured_ = settings;
  stop_.store(false);
  try {
    thread_ = std::thread(NetworkTask);
  } catch (...) {
    return false;
  }
  return true;
}

bool poll(LinkEvent* event) {
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

SendResult start(std::uint32_t generation, bool supersede) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (connection_state_ != ConnectionState::Online || stop_.load()) {
    return SendResult::Disconnected;
  }
  if (generation == 0 || generation <= generation_) {
    FailLocked("invalid_start");
    return SendResult::Disconnected;
  }
  AdvanceLocked(generation, TurnPhase::Uploading);
  if (outbound_.size() + kTransportFrames >= kQueueFrames) {
    FailLocked("retirement_overflow");
    return SendResult::Disconnected;
  }
  Outbound frame;
  frame.text = true;
  frame.retirement = true;
  const std::string text = "{\"type\":\"start\",\"generation\":" + std::to_string(generation) +
                           ",\"supersede\":" + (supersede ? "true}" : "false}");
  frame.size = text.size();
  std::copy(text.begin(), text.end(), frame.bytes.begin());
  outbound_.push_back(std::move(frame));
  changed_.notify_one();
  return SendResult::Ok;
}

SendResult send(std::uint32_t generation, const std::int16_t* pcm) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (connection_state_ != ConnectionState::Online || stop_.load()) {
    return SendResult::Disconnected;
  }
  if (pcm == nullptr || generation == 0 || generation != generation_ ||
      turn_phase_ != TurnPhase::Uploading || uplink_sequence_ >= 3000) {
    FailLocked("invalid_uplink");
    return SendResult::Disconnected;
  }
  if (outbound_.size() + kTransportFrames >= kQueueFrames) {
    return SendResult::Backpressure;
  }
  Outbound frame;
  frame.bytes = detail::EncodeAudio(generation, uplink_sequence_, pcm);
  frame.size = frame.bytes.size();
  outbound_.push_back(std::move(frame));
  ++uplink_sequence_;
  changed_.notify_one();
  return SendResult::Ok;
}

SendResult end(std::uint32_t generation) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (connection_state_ != ConnectionState::Online || stop_.load()) {
    return SendResult::Disconnected;
  }
  if (generation == 0 || generation != generation_ || turn_phase_ != TurnPhase::Uploading ||
      uplink_sequence_ == 0) {
    FailLocked("invalid_end");
    return SendResult::Disconnected;
  }
  if (outbound_.size() + kTransportFrames >= kQueueFrames) {
    return SendResult::Backpressure;
  }
  Outbound frame;
  frame.text = true;
  const std::string text =
      "{\"type\":\"end\",\"generation\":" + std::to_string(generation) + "}";
  frame.size = text.size();
  std::copy(text.begin(), text.end(), frame.bytes.begin());
  outbound_.push_back(std::move(frame));
  turn_phase_ = TurnPhase::WaitingReply;
  changed_.notify_one();
  return SendResult::Ok;
}

bool cancel(std::uint32_t generation, bool retract) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (connection_state_ != ConnectionState::Online || stop_.load()) {
    return false;
  }
  if (generation == 0 || generation <= generation_) {
    FailLocked("invalid_cancel");
    return false;
  }
  AdvanceLocked(generation, TurnPhase::Retired);
  if (outbound_.size() >= kQueueFrames) {
    FailLocked("retirement_overflow");
    return false;
  }

  std::string text =
      "{\"type\":\"cancel\",\"generation\":" + std::to_string(generation) + ",\"retract\":";
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

void close() noexcept {
  stop_.store(true);
  changed_.notify_all();
  if (thread_.joinable()) {
    thread_.join();
  }
  std::lock_guard<std::mutex> lock(mutex_);
  connection_state_ = ConnectionState::Offline;
  events_.clear();
  outbound_.clear();
}
}  // namespace boompi::voice_net
