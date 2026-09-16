#include "boompi/network/voice_net.h"

#include <openssl/asn1.h>
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
#include <stdexcept>
#include <thread>
#include <utility>
#include <websocketpp/client.hpp>
#include <websocketpp/config/asio_client.hpp>

#include "boompi/config/voice_client_config.h"
#include "network.h"
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

// 库会整批取走待发消息，get_buffered_amount不含当前async_write批次。
// 每批最多32帧；在途批次+待发队列合计最多约1.28s，同时容纳500ms句首突发。
constexpr std::size_t kQueueBytes = 32 * detail::kFrameBytes;
// 接收队列还包含字幕和控制事件，不只计 PCM；溢出时整条连接失败以免掩盖缺帧。
static constexpr std::size_t kEventCapacity = 64;
// 固定课堂 hello 口令，与只保存在服务端的 DashScope Key 无关。
static constexpr char kTeachingToken[] = "boompi-teaching-shared-token-v1-2026";

// Open 在线程启动前写入，此后只读；thread_ 的创建、join 由应用生命周期串行控制。
config::VoiceClientConfig configured_;
std::thread thread_;
std::atomic<bool> stop_{false};
// 除 stop_ 外，跨线程收发队列、连接状态和以下轮次字段都在 mutex_ 下访问。
std::mutex mutex_;
std::condition_variable changed_;
Client::connection_ptr connection_;
// 库的get_buffered_amount本版本没有加锁；只在网络线程采样，应用在同一锁下扣容量。
std::size_t buffered_bytes_{0};
std::deque<LinkEvent> events_;
// TLS pin 与底层发送计时只在网络线程访问，不需要应用线程读取。
std::array<unsigned char, 32> pin_{};
// 应用决定开始/取消；轮次号、上下行序号和旧结果隔离均由网络负责。
std::uint32_t generation_{0};
std::uint32_t uplink_sequence_{0};
std::uint32_t downlink_sequence_{0};
ConnectionState connection_state_{ConnectionState::Offline};
TurnPhase turn_phase_{TurnPhase::Retired};
bool audio_ended_{false};
void AdvanceLocked(std::uint32_t generation, TurnPhase phase) {
  generation_ = generation;
  turn_phase_ = phase;
  uplink_sequence_ = 0;
  downlink_sequence_ = 0;
  audio_ended_ = false;
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
  turn_phase_ = TurnPhase::Retired;
  events_.clear();
  // 只报告固定阶段码，不把原始 TLS/provider 消息或凭据带到应用日志。
  LinkEvent event;
  event.kind = LinkEventKind::Offline;
  event.data = code;
  events_.push_back(std::move(event));
  changed_.notify_all();
}

void Fail(const char* code) {
  std::lock_guard<std::mutex> lock(mutex_);
  FailLocked(code);
}

int VerifyPin(X509_STORE_CTX* store, void*) {
  X509* certificate = X509_STORE_CTX_get0_cert(store);
  if (!certificate || X509_check_purpose(certificate, X509_PURPOSE_SSL_SERVER, 0) <= 0) {
    return 0;
  }
  X509_PUBKEY* key = X509_get_X509_PUBKEY(certificate);
  std::array<unsigned char, 32> digest{};
  unsigned int size = 0;
  // 摘要覆盖完整SPKI；X509_pubkey_digest只处理裸公钥，不能替代此处。
  if (!key ||
      ASN1_item_digest(ASN1_ITEM_rptr(X509_PUBKEY), EVP_sha256(), key, digest.data(), &size) !=
          1 ||
      size != digest.size() || CRYPTO_memcmp(digest.data(), pin_.data(), digest.size()) != 0) {
    return 0;
  }
  X509_STORE_CTX_set_error(store, X509_V_OK);
  return 1;
}

websocketpp::lib::shared_ptr<TlsContext> MakeTls() {
  auto context = websocketpp::lib::make_shared<TlsContext>(TlsContext::tls_client);
  if (SSL_CTX_set_min_proto_version(context->native_handle(), TLS1_2_VERSION) != 1) {
    throw std::runtime_error("TLS initialization failed");
  }
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
  const std::string hello = "HELLO 4 16000 " + configured_.device_id + " " + kTeachingToken;
  websocketpp::lib::error_code error;
  client.send(handle, hello, websocketpp::frame::opcode::text, error);
  if (error) {
    Fail("hello_send");
  }
}

void OnMessage(Client::message_ptr message) {
  LinkEvent event;
  try {
    if (message->get_opcode() == websocketpp::frame::opcode::text) {
      event = detail::DecodeText(std::move(message->get_raw_payload()));
    } else if (message->get_opcode() == websocketpp::frame::opcode::binary) {
      event = detail::DecodeAudio(std::move(message->get_raw_payload()));
    } else {
      Fail("websocket_opcode");
      return;
    }
  } catch (...) {
    Fail("invalid_protocol");
    return;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  if (connection_state_ == ConnectionState::Failed || stop_.load()) {
    return;
  }
  // 握手、轮次、音频连续性在一个入口校验，错误统一结束连接。
  if (event.kind == LinkEventKind::Online) {
    if (connection_state_ == ConnectionState::Online) {
      FailLocked("duplicate_ready");
      return;
    }
    connection_state_ = ConnectionState::Online;
  } else {
    if (connection_state_ != ConnectionState::Online) {
      FailLocked("message_before_ready");
      return;
    }
    if (event.generation < generation_ ||
        (event.generation == generation_ && turn_phase_ == TurnPhase::Retired)) {
      return;
    }
    const bool audio = event.kind == LinkEventKind::Audio;
    if (!generation_ || event.generation != generation_ || turn_phase_ == TurnPhase::Complete ||
        (turn_phase_ == TurnPhase::Uploading && event.kind != LinkEventKind::Error) ||
        (audio && (audio_ended_ || event.sequence != downlink_sequence_))) {
      FailLocked("reply_order");
      return;
    }
    if (audio) {
      ++downlink_sequence_;
      audio_ended_ = event.data.size() < detail::kPcmBytes;
    } else if (event.kind == LinkEventKind::Done || event.kind == LinkEventKind::Error) {
      turn_phase_ = TurnPhase::Complete;
    }
  }
  if (events_.size() == kEventCapacity) {
    FailLocked("inbound_overflow");
    return;
  }
  events_.push_back(std::move(event));
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
  client.set_pong_timeout_handler([](Hdl, const std::string&) {
    Fail("heartbeat_timeout");
  });
  client.set_message_handler([](Hdl, Client::message_ptr message) {
    OnMessage(message);
  });
}

SendResult SendLocked(const void* bytes, std::size_t size,
                      websocketpp::frame::opcode::value opcode, bool critical = false) {
  if (connection_state_ != ConnectionState::Online || !connection_ || stop_.load()) {
    return SendResult::Disconnected;
  }
  if (buffered_bytes_ + size > kQueueBytes) {
    if (critical) {
      FailLocked("control_backpressure");  // 不能投递取消时断开，让服务端终止旧任务。
      return SendResult::Disconnected;
    }
    return SendResult::Backpressure;
  }
  if (connection_->send(bytes, size, opcode)) {
    FailLocked("send_failed");
    return SendResult::Disconnected;
  }
  buffered_bytes_ += size;
  changed_.notify_one();
  return SendResult::Ok;
}
// START与CANCEL都退休上一轮；递增、投递、清旧事件只在这里执行。
SendResult NewTurn(TurnPhase phase, bool retract) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (generation_ == UINT32_MAX) {
    FailLocked("generation_exhausted");
    return SendResult::Disconnected;
  }
  const auto generation = generation_ + 1;
  const std::string text = std::string(phase == TurnPhase::Uploading ? "START " : "CANCEL ") +
                           std::to_string(generation) + (retract ? " 1" : " 0");
  const auto result =
      SendLocked(text.data(), text.size(), websocketpp::frame::opcode::text, true);
  if (result == SendResult::Ok) {
    AdvanceLocked(generation, phase);
  }
  return result;
}

bool ProcessConnection(Client& client, Client::connection_ptr connection) {
  bool reached_ready = false;
  const auto started = Clock::now();
  auto ping_at = started;
  while (!stop_.load()) {
    // 1. 推进库自己的收发队列，不再转发一份自有PCM队列。
    client.poll();
    std::unique_lock<std::mutex> lock(mutex_);
    if (connection_state_ == ConnectionState::Failed) {
      break;
    }
    reached_ready = reached_ready || connection_state_ == ConnectionState::Online;
    // 2. 检查握手与心跳；断线交给外层网络任务重连。
    const auto now = Clock::now();
    // 所有库I/O回调都在本线程；持锁时应用不能send，读取库缓冲计数不会数据竞争。
    buffered_bytes_ = connection->get_buffered_amount();
    if (connection_state_ != ConnectionState::Online &&
        now - started > std::chrono::seconds(5)) {
      FailLocked("hello_timeout");
      break;
    }
    // pong超时交给WebSocket++；发ping间隔长于超时，不能覆盖尚未触发的定时器。
    if (connection_state_ == ConnectionState::Online &&
        now - ping_at >= std::chrono::seconds(10)) {
      websocketpp::lib::error_code error;
      connection->ping("boompi", error);
      if (error) {
        FailLocked("heartbeat_send");
        break;
      }
      ping_at = now;
    }
    changed_.wait_for(lock, std::chrono::milliseconds(2));
  }
  return reached_ready;
}

bool Connect(const network::Endpoint& selected) {
  const auto& endpoint = selected.server;
  std::array<unsigned char, 33> pin{};
  if (endpoint.server_spki_sha256.size() != 44 ||
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
  connection->set_pong_timeout(5000);
  connection->set_max_message_size(8192);
  {
    std::lock_guard<std::mutex> lock(mutex_);
    connection_ = connection;
    buffered_bytes_ = 0;
  }
  bool reached_ready = false;
  try {
    // 单一IPv4端点：自己发起一次async_connect，避免库的端点迭代关闭已绑定的socket。
    // TLS/HTTP/WebSocket握手仍由原连接的start()执行，不跳过证书验证。
    auto& socket = connection->get_raw_socket();
    socket.open(websocketpp::lib::asio::ip::tcp::v4());
    if (!network::bind_socket(static_cast<std::intptr_t>(socket.native_handle()), selected.interface)) {
      throw std::runtime_error("interface bind failed");
    }
    using Transport = Client::connection_type::transport_con_type;
    static_cast<Transport&>(*connection).set_uri(connection->get_uri());
    socket.async_connect(
        websocketpp::lib::asio::ip::tcp::endpoint(
            websocketpp::lib::asio::ip::address::from_string(endpoint.server_ip), endpoint.server_port),
        [connection](const websocketpp::lib::asio::error_code& ec) {
          if (ec) {
            Fail("tcp_connect");
          } else {
            connection->start();
          }
        });
    reached_ready = ProcessConnection(client, connection);
  } catch (...) {
    Fail("connection_io");
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    connection_.reset();  // client析构前撤回发送入口。
  }

  // 直接取消当前 socket，退出不等待远端完成 close 握手或 TCP/TLS 超时。
  websocketpp::lib::asio::error_code socket_error;
  connection->get_raw_socket().cancel(socket_error);
  connection->get_raw_socket().close(socket_error);
  client.stop_perpetual();
  client.stop();
  return reached_ready;
}

void NetworkTask() {
  bool wifi_first = false;
  while (!stop_.load()) {
    // 1. 清理上一条连接的数据，旧问题不会在重连后重新发送。
    {
      std::lock_guard<std::mutex> lock(mutex_);
      connection_state_ = ConnectionState::Offline;
      AdvanceLocked(generation_, TurnPhase::Retired);
    }
    try {
      // 2. 准备网卡、确定地址，然后持续处理这条 WSS 连接。
      network::Endpoint endpoint;
      if (!network::find_server(configured_, endpoint, stop_, wifi_first)) {
        Fail("network_setup");
      } else if (!stop_.load()) {
        const bool ready = Connect(endpoint);
        wifi_first = !ready && endpoint.interface && std::string(endpoint.interface) == "eth0";
      }
    } catch (...) {
      Fail("network_worker");
    }
    // 3. 连接结束后稍等再试；关闭请求可以提前唤醒等待。
    std::unique_lock<std::mutex> lock(mutex_);
    changed_.wait_for(lock, std::chrono::seconds(1), [] {
      return stop_.load();
    });
  }
}
}  // namespace

bool open(const config::VoiceClientConfig& settings) {
  if (thread_.joinable()) {
    return false;
  }
  generation_ = 0;
  configured_ = settings;
  stop_.store(false);
  try {
    thread_ = std::thread(NetworkTask);
  } catch (...) {
    return false;
  }
  return true;
}

bool online() {
  std::lock_guard<std::mutex> lock(mutex_);
  return connection_state_ == ConnectionState::Online;
}
bool uploading() {
  std::lock_guard<std::mutex> lock(mutex_);
  return turn_phase_ == TurnPhase::Uploading;
}
bool poll(LinkEvent& event) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (events_.empty()) {
    return false;
  }
  event = std::move(events_.front());
  events_.pop_front();
  return true;
}

SendResult start(bool supersede) {
  return NewTurn(TurnPhase::Uploading, supersede);
}

SendResult send(const audio::VoiceFrame16k& pcm) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (connection_state_ != ConnectionState::Online || stop_.load()) {
    return SendResult::Disconnected;
  }
  if (turn_phase_ != TurnPhase::Uploading || uplink_sequence_ == UINT32_MAX) {
    FailLocked("invalid_uplink");
    return SendResult::Disconnected;
  }
  const auto bytes = detail::EncodeAudio(generation_, uplink_sequence_, pcm);
  const auto result =
      SendLocked(bytes.data(), bytes.size(), websocketpp::frame::opcode::binary);
  if (result == SendResult::Ok) {
    ++uplink_sequence_;
  }
  return result;
}

SendResult end() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (connection_state_ != ConnectionState::Online || stop_.load()) {
    return SendResult::Disconnected;
  }
  if (turn_phase_ != TurnPhase::Uploading || uplink_sequence_ == 0) {
    FailLocked("invalid_end");
    return SendResult::Disconnected;
  }
  const std::string text = "END " + std::to_string(generation_);
  const auto result = SendLocked(text.data(), text.size(), websocketpp::frame::opcode::text);
  if (result == SendResult::Ok) {
    turn_phase_ = TurnPhase::WaitingReply;
  }
  return result;
}

bool cancel(bool retract) {
  return NewTurn(TurnPhase::Retired, retract) == SendResult::Ok;
}

void close() noexcept {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stop_.store(true);
    changed_.notify_all();
  }
  if (thread_.joinable()) {
    thread_.join();
  }
  std::lock_guard<std::mutex> lock(mutex_);
  connection_state_ = ConnectionState::Offline;
  turn_phase_ = TurnPhase::Retired;
  events_.clear();
}
}  // namespace boompi::voice_net
