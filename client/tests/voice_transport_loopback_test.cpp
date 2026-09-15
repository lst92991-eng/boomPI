/**
 * @file voice_transport_loopback_test.cpp
 * @brief 通过本机真实 TCP/TLS/WebSocket 对端验证产品 voice_net 的传输行为。
 *
 * 默认入口逐场景创建临时证书和回环监听端口，再运行 hello/ready、PCM、切代、
 * 故障、队列满和重连检查。只替换 network_setup 的板端网卡准备，不替换协议实现。
 * LoopbackServer 按测试脚本回包，不调用云模型；传入 host/port/pin 时改走外部测试
 * 服务 smoke 路径。实际麦克风、扬声器、课堂发现与人工问答体验仍需板端验收。
 */
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include <websocketpp/config/asio.hpp>
#include <websocketpp/server.hpp>

#include "boompi/network/voice_net.h"

namespace {

using Clock = std::chrono::steady_clock;
using boompi::config::VoiceClientConfig;
using boompi::voice_net::LinkEvent;
using boompi::voice_net::LinkEventKind;
using boompi::voice_net::SendResult;
namespace net = boompi::voice_net;

constexpr char kDeviceId[] = "00112233-4455-4677-8899-aabbccddeeff";

/// @brief 主测试路径检查失败时抛出包含场景原因的异常，由 main 汇总为失败退出码。
void Check(const bool condition, const char* const message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

// OpenSSL C 对象各有对应释放函数；这些删除器让测试异常退出时仍能完成资源回收。
struct BioDelete final {
  void operator()(BIO* value) const noexcept {
    BIO_free(value);
  }
};
struct KeyContextDelete final {
  void operator()(EVP_PKEY_CTX* value) const noexcept {
    EVP_PKEY_CTX_free(value);
  }
};
struct KeyDelete final {
  void operator()(EVP_PKEY* value) const noexcept {
    EVP_PKEY_free(value);
  }
};
struct CertificateDelete final {
  void operator()(X509* value) const noexcept {
    X509_free(value);
  }
};
struct ExtensionDelete final {
  void operator()(X509_EXTENSION* value) const noexcept {
    X509_EXTENSION_free(value);
  }
};

/**
 * @brief 每个回环服务独立生成的临时 TLS 身份，仅存内存，不依赖开发机证书文件。
 *
 * certificate/private_key 提供给测试服务器，spki_pin 提供给产品客户端进行真实校验。
 */
struct TestIdentity final {
  std::string certificate_pem;
  std::string private_key_pem;
  std::string spki_pin;
};

/// @brief 从内存 BIO 复制 PEM 文本，返回后即可释放 BIO，不把其内部指针留给服务器。
std::string BioText(BIO* const bio) {
  BUF_MEM* memory = nullptr;
  BIO_get_mem_ptr(bio, &memory);
  if (memory == nullptr || memory->data == nullptr || memory->length == 0U) {
    throw std::runtime_error("OpenSSL produced empty PEM");
  }
  return {memory->data, memory->length};
}

/**
 * @brief 生成 P-256 自签服务证书以及与其公钥匹配的 SHA-256 SPKI pin。
 *
 * serverAuth 用途对应产品 VerifyPin 的证书用途检查；证书只供本次测试，不写磁盘。
 * 客户端要验证真实握手公钥，不能仅比较测试脚本里两个预先设定的字符串。
 */
TestIdentity MakeTestIdentity() {
  std::unique_ptr<EVP_PKEY_CTX, KeyContextDelete> key_context(
      EVP_PKEY_CTX_new_id(EVP_PKEY_EC, nullptr));
  Check(
      key_context != nullptr && EVP_PKEY_keygen_init(key_context.get()) == 1 &&
          EVP_PKEY_CTX_set_ec_paramgen_curve_nid(key_context.get(), NID_X9_62_prime256v1) == 1,
      "could not initialize test TLS key");
  EVP_PKEY* raw_key = nullptr;
  Check(EVP_PKEY_keygen(key_context.get(), &raw_key) == 1 && raw_key != nullptr,
        "could not generate test TLS key");
  std::unique_ptr<EVP_PKEY, KeyDelete> key(raw_key);

  std::unique_ptr<X509, CertificateDelete> certificate(X509_new());
  Check(certificate != nullptr && X509_set_version(certificate.get(), 2L) == 1 &&
            ASN1_INTEGER_set(X509_get_serialNumber(certificate.get()), 1L) == 1 &&
            X509_gmtime_adj(X509_getm_notBefore(certificate.get()), -60L) != nullptr &&
            X509_gmtime_adj(X509_getm_notAfter(certificate.get()), 3600L) != nullptr &&
            X509_set_pubkey(certificate.get(), key.get()) == 1,
        "could not initialize test TLS certificate");
  X509_NAME* const subject = X509_get_subject_name(certificate.get());
  Check(
      subject != nullptr &&
          X509_NAME_add_entry_by_txt(
              subject, "CN", MBSTRING_ASC,
              reinterpret_cast<const unsigned char*>("boompi loopback test"), -1, -1, 0) == 1 &&
          X509_set_issuer_name(certificate.get(), subject) == 1,
      "could not name test TLS certificate");

  X509V3_CTX extension_context{};
  X509V3_set_ctx(&extension_context, certificate.get(), certificate.get(), nullptr, nullptr, 0);
  const auto add_extension = [&](const int nid, const char* const value) {
    std::unique_ptr<X509_EXTENSION, ExtensionDelete> extension(
        X509V3_EXT_conf_nid(nullptr, &extension_context, nid, const_cast<char*>(value)));
    Check(extension != nullptr && X509_add_ext(certificate.get(), extension.get(), -1) == 1,
          "could not add test TLS extension");
  };
  add_extension(NID_basic_constraints, "critical,CA:FALSE");
  add_extension(NID_key_usage, "critical,digitalSignature");
  add_extension(NID_ext_key_usage, "serverAuth");
  Check(X509_sign(certificate.get(), key.get(), EVP_sha256()) > 0,
        "could not sign test TLS certificate");

  std::unique_ptr<BIO, BioDelete> certificate_bio(BIO_new(BIO_s_mem()));
  std::unique_ptr<BIO, BioDelete> key_bio(BIO_new(BIO_s_mem()));
  Check(certificate_bio != nullptr && key_bio != nullptr &&
            PEM_write_bio_X509(certificate_bio.get(), certificate.get()) == 1 &&
            PEM_write_bio_PrivateKey(key_bio.get(), key.get(), nullptr, nullptr, 0, nullptr,
                                     nullptr) == 1,
        "could not encode test TLS identity");

  X509_PUBKEY* const public_key = X509_get_X509_PUBKEY(certificate.get());
  const int der_size = i2d_X509_PUBKEY(public_key, nullptr);
  Check(public_key != nullptr && der_size > 0, "could not size test SPKI");
  std::vector<unsigned char> der(static_cast<std::size_t>(der_size));
  unsigned char* cursor = der.data();
  Check(i2d_X509_PUBKEY(public_key, &cursor) == der_size, "could not encode test SPKI");
  std::array<unsigned char, 32> digest{};
  unsigned int digest_size = 0U;
  Check(EVP_Digest(der.data(), der.size(), digest.data(), &digest_size, EVP_sha256(),
                   nullptr) == 1 &&
            digest_size == digest.size(),
        "could not hash test SPKI");
  std::array<unsigned char, 45> encoded{};
  Check(EVP_EncodeBlock(encoded.data(), digest.data(), digest.size()) == 44,
        "could not encode test SPKI pin");
  return {BioText(certificate_bio.get()), BioText(key_bio.get()),
          std::string(reinterpret_cast<const char*>(encoded.data()), 44U)};
}

/// @brief 服务器收到的原始 opcode 与负载副本，供测试从线协议字节验证上行结果。
struct CapturedMessage final {
  websocketpp::frame::opcode::value opcode{};
  std::string payload;
};

/**
 * @brief 真实 WSS 测试对端；Asio 线程负责 socket，测试主线程只下发动作和读取快照。
 *
 * 服务端消息回调先保存收到的 hello/音频/CANCEL，再按配置自动响应 ready。
 * messages_/paused_ 用 mutex_ 和条件变量交接，handle_ 与连接对象在 Asio 线程操作。
 */
class LoopbackServer final {
  using Server = websocketpp::server<websocketpp::config::asio_tls>;
  using Hdl = websocketpp::connection_hdl;

 public:
  /**
   * @brief 绑定 127.0.0.1 的系统分配端口并启动 Asio 线程，避免固定端口相互冲突。
   * @param send_ready false 仍完成 TLS/WebSocket，只扣住业务 ready，用于握手超时场景。
   */
  explicit LoopbackServer(bool send_ready = true) : identity_(MakeTestIdentity()) {
    server_.clear_access_channels(websocketpp::log::alevel::all);
    server_.clear_error_channels(websocketpp::log::elevel::all);
    server_.init_asio();
    server_.set_reuse_addr(true);
    server_.set_tls_init_handler([this](Hdl) {
      auto context = websocketpp::lib::make_shared<websocketpp::lib::asio::ssl::context>(
          websocketpp::lib::asio::ssl::context::tls_server);
      SSL_CTX_set_min_proto_version(context->native_handle(), TLS1_2_VERSION);
      context->use_certificate_chain(websocketpp::lib::asio::buffer(identity_.certificate_pem));
      context->use_private_key(websocketpp::lib::asio::buffer(identity_.private_key_pem),
                               websocketpp::lib::asio::ssl::context::pem);
      return context;
    });
    server_.set_open_handler([this](Hdl handle) {
      handle_ = handle;
      websocketpp::lib::asio::error_code ec;
      server_.get_con_from_hdl(handle)->get_raw_socket().set_option(
          websocketpp::lib::asio::socket_base::receive_buffer_size(1024), ec);
      Check(!ec, "receive buffer setup failed");
    });
    server_.set_message_handler([this, send_ready](Hdl handle, Server::message_ptr message) {
      {
        std::lock_guard<std::mutex> lock(mutex_);
        messages_.push_back({message->get_opcode(), message->get_payload()});
      }
      changed_.notify_all();
      if (send_ready && message->get_opcode() == websocketpp::frame::opcode::text &&
          message->get_payload().find("\"type\":\"hello\"") != std::string::npos) {
        websocketpp::lib::error_code ec;
        server_.send(handle, "{\"type\":\"ready\",\"sample_rate\":16000,\"version\":3}",
                     websocketpp::frame::opcode::text, ec);
      }
    });
    websocketpp::lib::error_code ec;
    server_.listen(websocketpp::lib::asio::ip::tcp::endpoint(
                       websocketpp::lib::asio::ip::address_v4::loopback(), 0),
                   ec);
    Check(!ec, "listen failed");
    websocketpp::lib::asio::error_code endpoint_error;
    port_ = server_.get_local_endpoint(endpoint_error).port();
    Check(!endpoint_error && port_, "port lookup failed");
    server_.start_accept(ec);
    Check(!ec, "accept failed");
    thread_ = std::thread([this] {
      server_.run();
    });
  }

  /// @brief 先停止事件循环再 join，避免析构身份、消息队列后回调继续引用 this。
  ~LoopbackServer() {
    server_.stop();
    if (thread_.joinable()) {
      thread_.join();
    }
  }

  /// @brief 提供该实例的显式回环端点和正确 pin，绕过板端 DHCP/广播发现。
  VoiceClientConfig Config() const {
    return {kDeviceId, "127.0.0.1", port_, identity_.spki_pin};
  }

  /// @brief 将脚本回复 post 到 Asio 线程；允许发送故意构造的非法协议字节。
  void Send(std::string text, bool binary = false) {
    server_.get_io_service().post([this, text = std::move(text), binary] {
      websocketpp::lib::error_code ec;
      server_.send(
          handle_, text,
          binary ? websocketpp::frame::opcode::binary : websocketpp::frame::opcode::text, ec);
    });
  }

  /// @brief 从对端主动关闭当前 WebSocket，以验证客户端 Offline 后自动重新握手。
  void Drop() {
    server_.get_io_service().post([this] {
      websocketpp::lib::error_code ec;
      server_.close(handle_, websocketpp::close::status::going_away, "", ec);
    });
  }

  /**
   * @brief 在真实连接上暂停/恢复读取，并等 Asio 线程确认动作已经生效。
   *
   * 暂停配合较小接收缓冲制造 socket 背压，验证的是实际 TLS 传输积压而非假的队列。
   */
  void PauseReads(bool pause) {
    server_.get_io_service().post([this, pause] {
      websocketpp::lib::error_code ec;
      if (pause) {
        // WebSocket++ handle 是弱引用；暂停会移除通常持有连接的异步读取，
        // 因此暂停期间额外保存强引用，避免把连接被销毁误当作真实网络背压。
        paused_connection_ = server_.get_con_from_hdl(handle_);
        server_.pause_reading(handle_, ec);
      } else {
        server_.resume_reading(handle_, ec);
        paused_connection_.reset();
      }
      {
        std::lock_guard<std::mutex> lock(mutex_);
        paused_ = pause;
      }
      changed_.notify_all();
    });
    std::unique_lock<std::mutex> lock(mutex_);
    Check(changed_.wait_for(lock, std::chrono::seconds(2),
                            [this, pause] {
                              return paused_ == pause;
                            }),
          "could not change server read state");
  }

  /**
   * @brief 最多等三秒直到收到指定条数，返回包括早先消息的完整快照。
   *
   * hello 也计入条数，调用方须按线协议实际顺序索引；超时说明预期帧未送达。
   */
  std::vector<CapturedMessage> WaitMessages(std::size_t count) {
    std::unique_lock<std::mutex> lock(mutex_);
    Check(changed_.wait_for(lock, std::chrono::seconds(3),
                            [this, count] {
                              return messages_.size() >= count;
                            }),
          "expected uplink messages missing");
    return messages_;
  }

 private:
  TestIdentity identity_;
  Server server_;
  Hdl handle_;
  Server::connection_ptr paused_connection_;
  std::thread thread_;
  std::mutex mutex_;
  std::condition_variable changed_;
  std::vector<CapturedMessage> messages_;
  std::uint16_t port_{0};
  bool paused_{false};
};

// 测试退出必须join全局网络任务，即使断言抛出异常也不遗留线程。
struct NetworkScope {
  ~NetworkScope() {
    net::close();
  }
};
LinkEvent WaitEvent(unsigned timeout_ms = 3000) {
  const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
  LinkEvent event;
  while (Clock::now() < deadline) {
    if (net::poll(&event)) {
      return event;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  throw std::runtime_error("network event timeout");
}
void Open(const LoopbackServer& server) {
  const auto started = Clock::now();
  Check(net::open(server.Config()), "open rejected");
  Check(Clock::now() - started < std::chrono::milliseconds(200), "open blocked");
  Check(WaitEvent().kind == LinkEventKind::Online, "ready missing");
}
std::uint32_t Read32(const std::string& b, std::size_t offset) {
  const auto* p = reinterpret_cast<const unsigned char*>(b.data() + offset);
  return (std::uint32_t(p[0]) << 24U) | (std::uint32_t(p[1]) << 16U) |
         (std::uint32_t(p[2]) << 8U) | p[3];
}
std::string Audio(std::uint32_t generation, std::uint32_t sequence, std::size_t samples = 320) {
  std::string frame(12 + samples * 2, '\0');
  frame.replace(0, 4, "BPV3");
  for (unsigned i = 0; i < 4; ++i) {
    frame[4 + i] = static_cast<char>(generation >> (24 - i * 8));
    frame[8 + i] = static_cast<char>(sequence >> (24 - i * 8));
  }
  if (samples != 0) {
    frame[12] = 0x27;
  }
  return frame;
}
void Upload(std::uint32_t generation, bool supersede = false) {
  std::array<std::int16_t, 320> pcm{};
  pcm[0] = 0x1234;
  pcm[1] = -2;
  Check(net::start(generation, supersede) == SendResult::Ok, "start rejected");
  Check(net::send(generation, pcm.data()) == SendResult::Ok, "PCM rejected");
  Check(net::end(generation) == SendResult::Ok, "end rejected");
}
void BasicWireAndClose() {
  LoopbackServer server;
  NetworkScope scope;
  Open(server);
  Upload(1);
  const auto messages = server.WaitMessages(4);
  Check(messages[0].payload.find("\"version\":3") != std::string::npos &&
            messages[0].payload.find("\"sample_rate\":16000") != std::string::npos,
        "hello contract missing");
  Check(messages[1].payload == "{\"type\":\"start\",\"generation\":1,\"supersede\":false}",
        "START not separate");
  const auto& wire = messages[2].payload;
  Check(wire.size() == 652 && wire.substr(0, 4) == "BPV3" && Read32(wire, 4) == 1 &&
            Read32(wire, 8) == 0,
        "BPV3 header invalid");
  Check(static_cast<unsigned char>(wire[12]) == 0x34 &&
            static_cast<unsigned char>(wire[13]) == 0x12 &&
            static_cast<unsigned char>(wire[14]) == 0xfe &&
            static_cast<unsigned char>(wire[15]) == 0xff,
        "PCM byte order");
  Check(messages[3].payload == "{\"type\":\"end\",\"generation\":1}", "END missing");
  server.Send("{\"type\":\"text\",\"generation\":1,\"text\":\"你好\"}");
  Check(WaitEvent().text == "你好", "text mismatch");
  server.Send(Audio(1, 0), true);
  Check(WaitEvent().audio_size == 640, "full audio missing before DONE");
  server.Send(Audio(1, 1, 7), true);
  Check(WaitEvent().audio_size == 14, "short tail lost");
  server.Send("{\"type\":\"done\",\"generation\":1}");
  Check(WaitEvent().kind == LinkEventKind::Done, "DONE missing");
  const auto began = Clock::now();
  net::close();
  net::close();
  Check(Clock::now() - began < std::chrono::milliseconds(500), "close blocked");
  LinkEvent event;
  Check(!net::poll(&event), "close retained events");
}
void CancelAfterEndAndOldGeneration() {
  for (bool playing : {false, true}) {
    LoopbackServer server;
    NetworkScope scope;
    Open(server);
    Upload(1);
    server.WaitMessages(4);
    if (playing) {
      server.Send(Audio(1, 0), true);
      Check(WaitEvent().kind == LinkEventKind::Audio, "playback setup");
    }
    Check(net::cancel(2, true), "cancel after END rejected");
    Upload(3);
    const auto messages = server.WaitMessages(8);
    Check(messages[4].payload == "{\"type\":\"cancel\",\"generation\":2,\"retract\":true}",
          "cancel lost before START");
    server.Send(Audio(1, 8, 2), true);
    server.Send("{\"type\":\"done\",\"generation\":1}");
    server.Send(Audio(3, 0, 2), true);
    server.Send("{\"type\":\"done\",\"generation\":3}");
    Check(WaitEvent().generation == 3, "old PCM resurrected");
    Check(WaitEvent().kind == LinkEventKind::Done, "new done missing");
  }
}
void ErrorEndsUpload() {
  LoopbackServer server;
  NetworkScope scope;
  Open(server);
  std::array<std::int16_t, 320> pcm{};
  Check(net::start(1, false) == SendResult::Ok && net::send(1, pcm.data()) == SendResult::Ok,
        "input rejected");
  server.WaitMessages(3);
  server.Send("{\"type\":\"error\",\"generation\":1,\"code\":\"provider_error\"}");
  Check(WaitEvent().kind == LinkEventKind::Error, "input error rejected");
  Check(net::send(1, pcm.data()) == SendResult::Disconnected, "failed upload continued");
}
void RejectBrokenWire() {
  for (unsigned scenario = 0; scenario < 8; ++scenario) {
    LoopbackServer server;
    NetworkScope scope;
    Open(server);
    Upload(1);
    server.WaitMessages(4);
    if (scenario == 0) {
      auto frame = Audio(1, 0);
      frame[3] = '2';
      server.Send(frame, true);
    }
    if (scenario == 1) {
      auto frame = Audio(1, 0);
      frame.pop_back();
      server.Send(frame, true);
    }
    if (scenario == 2) {
      server.Send(Audio(2, 0), true);
    }
    if (scenario == 3) {
      server.Send(Audio(1, 1), true);
    }
    if (scenario == 4) {
      server.Send(Audio(1, 0, 2), true);
      Check(WaitEvent().kind == LinkEventKind::Audio, "short setup");
      server.Send(Audio(1, 1), true);
    }
    if (scenario == 5) {
      server.Send("{\"type\":\"ready\",\"sample_rate\":16000,\"version\":3}");
    }
    if (scenario == 6) {
      server.Send("{\"type\":\"done\",\"generation\":1}");
      Check(WaitEvent().kind == LinkEventKind::Done, "empty DONE setup");
      server.Send(Audio(1, 0), true);
    }
    if (scenario == 7) {
      server.Send(Audio(1, 0, 321), true);
    }
    Check(WaitEvent().kind == LinkEventKind::Offline, "invalid wire accepted");
  }
}
void TlsFailureAndReconnect() {
  LoopbackServer server;
  NetworkScope scope;
  auto wrong = server.Config();
  wrong.server_spki_sha256[0] = wrong.server_spki_sha256[0] == 'A' ? 'B' : 'A';
  Check(net::open(wrong) && WaitEvent().kind == LinkEventKind::Offline, "wrong pin accepted");
  net::close();
  Open(server);
  Upload(1);
  server.WaitMessages(4);
  server.Drop();
  Check(WaitEvent().kind == LinkEventKind::Offline, "disconnect missing");
  Check(WaitEvent().kind == LinkEventKind::Online, "reconnect missing");
  Upload(5);
  server.Send(Audio(1, 0, 2), true);
  server.Send("{\"type\":\"done\",\"generation\":5}");
  Check(WaitEvent().generation == 5, "old generation survived reconnect");
}
void BoundedQueuesAndRetirement() {
  LoopbackServer server;
  NetworkScope scope;
  Open(server);
  server.PauseReads(true);
  std::array<std::int16_t, 320> pcm{};
  Check(net::start(1, false) == SendResult::Ok, "start blocked");
  const auto began = Clock::now();
  bool full = false;
  for (unsigned i = 0; i < 1000; ++i) {
    auto result = net::send(1, pcm.data());
    if (result == SendResult::Backpressure) {
      full = true;
      break;
    }
    Check(result == SendResult::Ok, "queue disconnected instead of backpressure");
  }
  Check(full && Clock::now() - began < std::chrono::milliseconds(200),
        "unbounded or blocking PCM queue");
  Check(net::cancel(2, true), "cancel reserve lost");
  Check(net::start(3, true) == SendResult::Ok, "supersede reserve lost");
  Check(net::cancel(4, false), "second cancel reserve lost");
  Upload(5);
  server.PauseReads(false);
  unsigned stage = 0;
  std::size_t count = 1;
  for (unsigned i = 0; i < 100 && stage < 4; ++i) {
    auto messages = server.WaitMessages(++count);
    const auto& message = messages[count - 1];
    if (message.opcode == websocketpp::frame::opcode::binary) {
      continue;
    }
    if (message.payload.find("\"type\":\"hello\"") != std::string::npos) {
      Check(WaitEvent().kind == LinkEventKind::Offline, "silent send loss");
      return;
    }
    const std::array<std::string, 4> expected = {
        "{\"type\":\"cancel\",\"generation\":2,\"retract\":true}",
        "{\"type\":\"start\",\"generation\":3,\"supersede\":true}",
        "{\"type\":\"cancel\",\"generation\":4,\"retract\":false}",
        "{\"type\":\"start\",\"generation\":5,\"supersede\":false}"};
    if (stage == 0 &&
        message.payload == "{\"type\":\"start\",\"generation\":1,\"supersede\":false}") {
      continue;
    }
    Check(message.payload == expected[stage], "retirement reordered or lost");
    ++stage;
  }
  Check(stage == 4, "retirement sequence incomplete");
}
void RejectOldHandshake() {
  for (const char* ready :
       {"{\"type\":\"ready\"}", "{\"type\":\"ready\",\"sample_rate\":16000}",
        "{\"type\":\"ready\",\"sample_rate\":24000,\"version\":3}",
        "{\"type\":\"ready\",\"sample_rate\":16000,\"version\":2}"}) {
    LoopbackServer server(false);
    NetworkScope scope;
    Check(net::open(server.Config()), "open failed");
    server.WaitMessages(1);
    server.Send(ready);
    Check(WaitEvent().kind == LinkEventKind::Offline, "old handshake accepted");
  }
  LoopbackServer server(false);
  NetworkScope scope;
  Check(net::open(server.Config()), "open failed");
  const auto event = WaitEvent(6500);
  Check(event.kind == LinkEventKind::Offline && event.code == "hello_timeout",
        "hello timeout missing");
}
void RejectInvalidInputOrder() {
  for (unsigned scenario = 0; scenario < 3; ++scenario) {
    LoopbackServer server;
    NetworkScope scope;
    Open(server);
    std::array<std::int16_t, 320> pcm{};
    if (scenario == 0) {
      Check(net::send(1, pcm.data()) == SendResult::Disconnected, "PCM before START accepted");
    } else {
      Check(net::start(1, false) == SendResult::Ok, "START failed");
      if (scenario == 1) {
        Check(net::end(1) == SendResult::Disconnected, "empty END accepted");
      } else {
        Check(net::start(1, false) == SendResult::Disconnected, "generation reused");
      }
    }
    Check(WaitEvent().kind == LinkEventKind::Offline, "input failure not reported");
  }
}

void ExternalServerSmoke(const char* host, const char* port, const char* pin) {
  NetworkScope scope;
  const auto parsed = std::stoul(port);
  Check(parsed > 0 && parsed <= 65535, "invalid port");
  VoiceClientConfig config{kDeviceId, host, static_cast<std::uint16_t>(parsed), pin};
  auto wrong = config;
  wrong.server_spki_sha256[0] = wrong.server_spki_sha256[0] == 'A' ? 'B' : 'A';
  Check(net::open(wrong) && WaitEvent().kind == LinkEventKind::Offline,
        "server accepted wrong pin");
  net::close();
  Check(net::open(config) && WaitEvent().kind == LinkEventKind::Online, "server not ready");
  std::array<std::int16_t, 320> pcm{};
  pcm[0] = 0x0807;
  Check(net::start(1, false) == SendResult::Ok && net::send(1, pcm.data()) == SendResult::Ok &&
            net::end(1) == SendResult::Ok,
        "smoke input");
  bool text = false, audio = false, done = false;
  for (unsigned i = 0; i < 8 && !done; ++i) {
    auto event = WaitEvent();
    if (event.kind == LinkEventKind::Text) {
      text = event.text == "你好";
    } else if (event.kind == LinkEventKind::Audio) {
      audio = event.audio_size == 4;
    } else if (event.kind == LinkEventKind::Done) {
      done = true;
    } else {
      throw std::runtime_error("smoke failed");
    }
  }
  Check(text && audio && done, "smoke incomplete");
  std::cout << "WSS_SMOKE_OK\n";
}
}  // namespace
int main(int argc, char** argv) {
  try {
    if (argc == 4) {
      ExternalServerSmoke(argv[1], argv[2], argv[3]);
      return EXIT_SUCCESS;
    }
    BasicWireAndClose();
    CancelAfterEndAndOldGeneration();
    ErrorEndsUpload();
    RejectBrokenWire();
    TlsFailureAndReconnect();
    BoundedQueuesAndRetirement();
    RejectOldHandshake();
    RejectInvalidInputOrder();
  } catch (const std::exception& error) {
    net::close();
    std::cerr << "voice net loopback: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
  std::cout << "voice net loopback: passed\n";
  return EXIT_SUCCESS;
}
