/**
 * @file voice_transport_loopback_test.cpp
 * @brief 通过本机真实 TCP/TLS/WebSocket 对端验证产品 VoiceLink 的传输行为。
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

#include "boompi/network/voice_link.h"

namespace {

using Clock = std::chrono::steady_clock;
using boompi::config::VoiceClientConfig;
using boompi::network::LinkEvent;
using boompi::network::LinkEventKind;
using boompi::network::SendResult;
using boompi::network::VoiceLink;

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
 * 服务端消息回调先保存收到的 hello/音频/STOP，再按配置自动响应 ready。
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
        server_.send(handle, "{\"type\":\"ready\",\"sample_rate\":16000}",
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

/**
 * @brief 在测试线程以 1 ms 间隔轮询下一条事件，给异步链路有限时间完成。
 *
 * 不按类型筛掉中间事件，避免把意外 Offline 或旧代回复吞掉后仍报告场景通过。
 */
LinkEvent WaitEvent(VoiceLink& link, unsigned timeout_ms = 3000) {
  const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
  LinkEvent event;
  while (Clock::now() < deadline) {
    if (link.PollEvent(&event)) {
      return event;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  throw std::runtime_error("link event timeout");
}

/// @brief 同时验证 Open 本身快速返回，以及真正 ready 才映射为第一个 Online 事件。
void Open(VoiceLink& link, const LoopbackServer& server) {
  const auto started = Clock::now();
  Check(link.Open(server.Config()), "Open rejected");
  Check(Clock::now() - started < std::chrono::milliseconds(200), "Open blocked on networking");
  Check(WaitEvent(link).kind == LinkEventKind::Online, "ready did not become Online");
}

/// @brief 测试端独立读网络大端头字段，不调用产品解码器来验证产品自己的输出。
std::uint32_t Read32(const std::string& bytes, std::size_t offset) {
  const auto* p = reinterpret_cast<const unsigned char*>(bytes.data() + offset);
  return (static_cast<std::uint32_t>(p[0]) << 24U) | (static_cast<std::uint32_t>(p[1]) << 16U) |
         (static_cast<std::uint32_t>(p[2]) << 8U) | p[3];
}

/**
 * @brief 独立构造服务器下行字节，不调用产品编码器，避免编码与解码共享错误而自洽。
 *
 * samples 默认 320，对应 16 kHz 下行 20 ms；flags/sequence/长度可故意传错以测试拒绝。
 */
std::string Audio(std::uint32_t generation, std::uint32_t sequence, unsigned flags,
                  std::size_t samples = 320) {
  std::string frame(16 + samples * 2, '\0');
  frame.replace(0, 4, "BPV2");
  frame[5] = static_cast<char>(flags);
  for (unsigned i = 0; i < 4; ++i) {
    frame[8 + i] = static_cast<char>(generation >> (24 - i * 8));
    frame[12 + i] = static_cast<char>(sequence >> (24 - i * 8));
  }
  frame[16] = 0x27;
  return frame;
}

/// @brief 提交一帧即结束的输入；正数 0x1234 和负数 -2 用于验证 PCM 小端与符号编码。
void Upload(VoiceLink& link, std::uint32_t generation, bool supersede = false) {
  std::array<std::int16_t, 320> pcm{};
  pcm[0] = 0x1234;
  pcm[1] = -2;
  Check(link.SendAudio(generation, pcm.data(), true, true, supersede) == SendResult::Ok,
        "complete input was rejected");
}

/**
 * @brief 正常链路：hello → 一帧完整问题 → 字幕/满帧音频/短尾帧/done → 有界关闭。
 *
 * 同时检查上行 656 字节头与 PCM 字节序、下行事件顺序和末帧有效长度；Close 后不得
 * 留下上一连接事件。此处收到音频只验证传输交付，不代表已送入真实 ALSA 扬声器。
 */
void BasicWireAndClose() {
  LoopbackServer server;
  VoiceLink link;
  Open(link, server);
  Upload(link, 1);
  const auto messages = server.WaitMessages(2);
  Check(messages[0].payload == "{\"type\":\"hello\",\"device_id\":\"" + std::string(kDeviceId) +
                                   "\",\"token\":\"boompi-teaching-shared-token-v1-2026\","
                                   "\"sample_rate\":16000}",
        "hello schema mismatch");
  const auto& wire = messages[1].payload;
  Check(messages[1].opcode == websocketpp::frame::opcode::binary && wire.size() == 656 &&
            wire.substr(0, 4) == "BPV2" && wire[5] == 3 && Read32(wire, 8) == 1 &&
            Read32(wire, 12) == 0 && static_cast<unsigned char>(wire[16]) == 0x34 &&
            static_cast<unsigned char>(wire[17]) == 0x12 &&
            static_cast<unsigned char>(wire[18]) == 0xfe &&
            static_cast<unsigned char>(wire[19]) == 0xff,
        "PCM wire contract mismatch");
  server.Send("{\"type\":\"text\",\"generation\":1,\"text\":\"你好\"}");
  server.Send(Audio(1, 0, 1), true);
  server.Send(Audio(1, 1, 2, 7), true);
  server.Send("{\"type\":\"done\",\"generation\":1}");
  auto event = WaitEvent(link);
  Check(event.kind == LinkEventKind::Text && event.text == "你好", "text lost");
  event = WaitEvent(link);
  Check(event.kind == LinkEventKind::Audio && event.start && !event.end &&
            event.audio_size == 640,
        "first audio lost");
  event = WaitEvent(link);
  Check(
      event.kind == LinkEventKind::Audio && !event.start && event.end && event.audio_size == 14,
      "tail audio lost");
  Check(WaitEvent(link).kind == LinkEventKind::Done, "done missing");
  const auto close_started = Clock::now();
  link.Close();
  Check(Clock::now() - close_started < std::chrono::milliseconds(500), "Close was not bounded");
  Check(!link.PollEvent(&event), "Close kept stale events");
}

/**
 * @brief 先开第 1 代，再以第 2 代 SUPERSEDE，故意让旧字幕/音频/done 晚到。
 *
 * 应用只能看到第 2 代回复；随后 STOP 使用第 3 代退休，再开第 4 代验证旧代不会复活。
 * 对第 2 代只回文本和 done，也覆盖无音频回答可正常完成的协议路径。
 */
void GenerationFence() {
  LoopbackServer server;
  VoiceLink link;
  Open(link, server);
  Upload(link, 1);
  server.WaitMessages(2);
  Upload(link, 2, true);
  const auto wire = server.WaitMessages(3);
  Check(wire[2].payload[5] == 7 && Read32(wire[2].payload, 8) == 2,
        "supersede START flags wrong");
  server.Send("{\"type\":\"text\",\"generation\":1,\"text\":\"stale\"}");
  server.Send(Audio(1, 0, 3), true);
  server.Send("{\"type\":\"done\",\"generation\":1}");
  server.Send("{\"type\":\"text\",\"generation\":2,\"text\":\"new\"}");
  server.Send("{\"type\":\"done\",\"generation\":2}");
  auto event = WaitEvent(link);
  Check(event.kind == LinkEventKind::Text && event.generation == 2 && event.text == "new",
        "old generation leaked");
  Check(WaitEvent(link).kind == LinkEventKind::Done, "text-only done missing");
  Check(link.Stop(3, true), "STOP rejected");
  const auto stopped = server.WaitMessages(4);
  Check(stopped[3].payload == "{\"type\":\"stop\",\"generation\":3,\"retract\":true}",
        "STOP schema mismatch");
  server.Send("{\"type\":\"done\",\"generation\":2}");
  Upload(link, 4);
  server.WaitMessages(5);
  server.Send("{\"type\":\"done\",\"generation\":4}");
  event = WaitEvent(link);
  Check(event.kind == LinkEventKind::Done && event.generation == 4,
        "STOP retired generation revived");
}

// 上传 END 只结束输入：等待首包和正在收音频时，STOP 都必须立即隔离旧回答。
void StopAfterEnd() {
  for (const bool reply_started : {false, true}) {
    LoopbackServer server;
    VoiceLink link;
    Open(link, server);
    Upload(link, 1);
    server.WaitMessages(2);
    if (reply_started) {
      server.Send(Audio(1, 0, 1), true);
      Check(WaitEvent(link).kind == LinkEventKind::Audio, "reply did not start");
    }
    Check(link.Stop(2, true), "END disabled cancellation");
    const auto stopped = server.WaitMessages(3);
    Check(stopped[2].payload == "{\"type\":\"stop\",\"generation\":2,\"retract\":true}",
          "END cancellation was not sent");
    server.Send(Audio(1, reply_started ? 1 : 0, reply_started ? 2 : 3, 2), true);
    server.Send("{\"type\":\"text\",\"generation\":1,\"text\":\"stale\"}");
    server.Send("{\"type\":\"done\",\"generation\":1}");
    Upload(link, 3);
    server.WaitMessages(4);
    server.Send("{\"type\":\"done\",\"generation\":3}");
    const auto done = WaitEvent(link);
    Check(done.kind == LinkEventKind::Done && done.generation == 3,
          "cancelled reply escaped into next turn");
  }
}

// 云端可在上传过程中失败。Error 已结束该轮，不能再把它当成仍开放的输入。
void ErrorEndsUpload() {
  LoopbackServer server;
  VoiceLink link;
  Open(link, server);
  std::array<std::int16_t, 320> pcm{};
  Check(link.SendAudio(1, pcm.data(), true, false, false) == SendResult::Ok,
        "input did not start");
  server.WaitMessages(2);
  server.Send("{\"type\":\"error\",\"generation\":1,\"code\":\"provider_timeout\"}");
  const auto error = WaitEvent(link);
  Check(error.kind == LinkEventKind::Error && error.generation == 1,
        "upload error was not delivered");
  Check(link.SendAudio(1, pcm.data(), false, true, false) == SendResult::Disconnected,
        "provider error left upload open");
}

/**
 * @brief 每次新建连接后注入一类坏帧，要求显式 Offline，不能静默补帧或继续播放。
 *
 * 覆盖 sequence hole、音频 END 前 done、END 后音频、未来 generation、重复 ready、
 * 重复 JSON 键及非末帧长度不足；允许坏帧前的合法事件已被测试线程先取走。
 */
void RejectBrokenWire() {
  const std::vector<std::vector<std::pair<std::string, bool>>> cases{
      {{Audio(1, 0, 1), true}, {Audio(1, 2, 2), true}},
      {{Audio(1, 0, 1), true}, {"{\"type\":\"done\",\"generation\":1}", false}},
      {{Audio(1, 0, 3), true}, {Audio(1, 1, 2), true}},
      {{"{\"type\":\"text\",\"generation\":2,\"text\":\"future\"}", false}},
      {{"{\"type\":\"ready\",\"sample_rate\":16000}", false}},
      {{"{\"type\":\"text\",\"generation\":1,\"generation\":1,\"text\":\"duplicate\"}", false}},
      {{Audio(1, 0, 1, 1), true}},
      {{Audio(1, 0, 3, 480), true}},  // 旧版 24 kHz 满帧不能当成 16 kHz 接收。
  };
  for (const auto& messages : cases) {
    LoopbackServer server;
    VoiceLink link;
    Open(link, server);
    Upload(link, 1);
    server.WaitMessages(2);
    for (const auto& message : messages) {
      server.Send(message.first, message.second);
    }
    bool offline = false;
    for (unsigned i = 0; i < 3 && !offline; ++i) {
      offline = WaitEvent(link).kind == LinkEventKind::Offline;
    }
    Check(offline, "invalid wire did not close the connection");
  }
}

/**
 * @brief 错误 pin 必须在握手期失败；正确 pin 连通后，对端关闭应触发 Offline→Online。
 *
 * 新连接再上传第 5 代，确认重连不要求 actor 重用旧代号或重新构造 VoiceLink。
 */
void TlsFailureAndReconnect() {
  LoopbackServer server;
  VoiceLink wrong_pin;
  auto config = server.Config();
  config.server_spki_sha256[0] = config.server_spki_sha256[0] == 'A' ? 'B' : 'A';
  Check(wrong_pin.Open(config), "TLS failure did not start");
  auto event = WaitEvent(wrong_pin);
  Check(event.kind == LinkEventKind::Offline && event.code == "tls_connect",
        "pin mismatch not reported");
  wrong_pin.Close();

  VoiceLink link;
  Open(link, server);
  Upload(link, 1);
  server.WaitMessages(2);
  server.Drop();
  Check(WaitEvent(link).kind == LinkEventKind::Offline, "disconnect not reported");
  Check(WaitEvent(link).kind == LinkEventKind::Online, "automatic reconnect failed");
  const auto reconnected = server.WaitMessages(3);
  Check(reconnected[2].payload.find("\"type\":\"hello\"") != std::string::npos,
        "reconnect replayed old upload");
  Upload(link, 5);
  server.WaitMessages(4);
  server.Send(Audio(1, 0, 3, 2), true);
  server.Send("{\"type\":\"done\",\"generation\":1}");
  server.Send("{\"type\":\"done\",\"generation\":5}");
  const auto done = WaitEvent(link);
  Check(done.kind == LinkEventKind::Done && done.generation == 5,
        "reconnect revived the retired turn");
}

/**
 * @brief 快速投递填满上行队列，要求及时 Backpressure，同时仍能排入 STOP 和新代。
 *
 * 后半段停止消费并发送 65 条字幕，超过 64 条事件容量后应明确 inbound_overflow。
 * 两个方向的容量失败都不能以“成功但丢数据”掩盖，切代后旧代 done 也不得穿透。
 */
void BoundedQueuesAndSupersede() {
  LoopbackServer server;
  VoiceLink link;
  Open(link, server);
  std::array<std::int16_t, 320> pcm{};
  bool full = false;
  const auto started = Clock::now();
  for (unsigned i = 0; i < 200000; ++i) {
    const auto result = link.SendAudio(1, pcm.data(), i == 0, false, false);
    if (result == SendResult::Backpressure) {
      full = true;
      break;
    }
    Check(result == SendResult::Ok, "burst input failed before capacity");
  }
  Check(full && Clock::now() - started < std::chrono::milliseconds(250),
        "SendAudio blocks or fails to bound burst input");
  Check(link.Stop(2, false), "STOP reserve unavailable at full input queue");
  Upload(link, 3, true);
  server.Send("{\"type\":\"done\",\"generation\":1}");
  // 新代同时淘汰尚在本地排队的旧普通 PCM 和服务器迟到的旧回答。
  server.Send("{\"type\":\"done\",\"generation\":3}");
  const auto done = WaitEvent(link);
  Check(done.kind == LinkEventKind::Done && done.generation == 3, "full-queue fence failed");

  // 提交第 4 代完整输入后暂停应用消费，让足够多的输出填满有界事件队列。
  Upload(link, 4);
  for (unsigned i = 0; i < 65; ++i) {
    server.Send("{\"type\":\"text\",\"generation\":4,\"text\":\"x\"}");
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  const auto offline = WaitEvent(link);
  Check(offline.kind == LinkEventKind::Offline && offline.code == "inbound_overflow",
        "receive overflow was hidden");
}

/// @brief 对端完成 WSS 却不发 ready，要求约五秒后 hello_timeout，不能一直等待业务就绪。
void HandshakeTimeout() {
  LoopbackServer server(false);
  VoiceLink link;
  Check(link.Open(server.Config()), "no-ready Open rejected");
  const auto event = WaitEvent(link, 6500);
  Check(event.kind == LinkEventKind::Offline && event.code == "hello_timeout",
        "missing ready was not bounded");
}

// 采样率在握手阶段确认；旧 ready 或声明 24 kHz 都不能进入 Online。
void RejectOldSampleRate() {
  for (const char* ready :
       {"{\"type\":\"ready\"}", "{\"type\":\"ready\",\"sample_rate\":24000}"}) {
    LoopbackServer server(false);
    VoiceLink link;
    Check(link.Open(server.Config()), "rate handshake did not start");
    server.WaitMessages(1);
    server.Send(ready);
    const auto event = WaitEvent(link);
    Check(event.kind == LinkEventKind::Offline && event.code == "invalid_protocol",
          "legacy audio rate reached Online");
  }
}

/**
 * @brief 连续切代时，验证尚未发出的 STOP/SUPERSEDE 仍按顺序传达退休与撤回含义。
 *
 * 先测紧邻提交，再暂停真实 TLS 读取制造积压；旧普通 PCM 可淘汰，但退休标记须保序。
 * 若真实 TCP 阻塞超过发送期限，允许有界 Offline；不允许标记悄悄丢失仍声称成功。
 */
void PendingRetirementKeepsMeaning() {
  LoopbackServer server;
  VoiceLink link;
  Open(link, server);
  // 两个操作紧接提交，中间不等 ACK 或 socket；即使仍在排队，
  // retract STOP 也必须先于随后的普通 START 抵达，才能保留撤回旧回答的含义。
  Upload(link, 1);
  server.WaitMessages(2);
  Check(link.Stop(2, true), "pending retract STOP failed");
  Upload(link, 3);
  auto messages = server.WaitMessages(4);
  Check(messages[2].payload == "{\"type\":\"stop\",\"generation\":2,\"retract\":true}" &&
            Read32(messages[3].payload, 8) == 3,
        "normal START erased pending history retraction");

  // START|SUPERSEDE 同样携带撤回旧回答历史的作用；紧接的 STOP 清理旧 PCM 时不能删掉它。
  Upload(link, 4, true);
  Check(link.Stop(5, false), "stop after pending supersede failed");
  Upload(link, 6);
  messages = server.WaitMessages(7);
  Check(messages[4].payload[5] == 7 && Read32(messages[4].payload, 8) == 4 &&
            messages[5].payload == "{\"type\":\"stop\",\"generation\":5,\"retract\":false}" &&
            Read32(messages[6].payload, 8) == 6,
        "new fence erased supersede history retraction");

  // 暂停真实 TLS 读取，用较小接收窗口使 socket 发送受阻；
  // 此时普通旧 PCM 可退休，STOP 与 SUPERSEDE 仍须保持顺序。
  server.PauseReads(true);
  std::array<std::int16_t, 320> pcm{};
  bool full = false;
  for (unsigned i = 0; i < 1000 && !full; ++i) {
    const auto result = link.SendAudio(7, pcm.data(), i == 0, false, false);
    full = result == SendResult::Backpressure;
    if (result == SendResult::Disconnected) {
      const auto failure = WaitEvent(link);
      throw std::runtime_error("slow reader disconnected before backpressure at " +
                               std::to_string(i) + ": " + failure.code);
    }
    if (!full) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }
  Check(full, "slow reader did not fill bounded queue");
  const auto fence_started = Clock::now();
  Check(link.Stop(8, true), "blocked writer lost STOP reserve");
  Upload(link, 9, true);
  Check(link.Stop(10, false), "blocked writer lost supersede marker");
  Upload(link, 11);
  server.PauseReads(false);
  std::size_t count = 7;
  unsigned stage = 0;
  while (stage < 4) {
    messages = server.WaitMessages(++count);
    const auto& message = messages[count - 1];
    if (message.opcode == websocketpp::frame::opcode::text &&
        message.payload.find("\"type\":\"hello\"") != std::string::npos) {
      // TCP 零窗口探测可能超过 800 ms 的退休帧发送期限；重连出现新的 hello 时，
      // 必须已经给应用报告有界失败，不能把重连当作先前退休标记已经送达。
      const auto failure = WaitEvent(link);
      Check(failure.kind == LinkEventKind::Offline &&
                (failure.code == "send_timeout" || failure.code == "uplink_timeout") &&
                Clock::now() - fence_started < std::chrono::seconds(2),
            "blocked retirement did not report bounded send failure");
      return;
    }
    if (message.opcode == websocketpp::frame::opcode::binary &&
        Read32(message.payload, 8) == 7) {
      continue;
    }
    if (stage == 0 &&
        message.payload != "{\"type\":\"stop\",\"generation\":8,\"retract\":true}") {
      throw std::runtime_error(
          "blocked STOP order: opcode=" + std::to_string(message.opcode) +
          (message.opcode == websocketpp::frame::opcode::binary
               ? ", generation=" + std::to_string(Read32(message.payload, 8))
               : ", text=" + message.payload));
    }
    if (stage == 1) {
      Check(message.opcode == websocketpp::frame::opcode::binary && message.payload[5] == 7 &&
                Read32(message.payload, 8) == 9,
            "blocked supersede erased");
    }
    if (stage == 2) {
      Check(message.payload == "{\"type\":\"stop\",\"generation\":10,\"retract\":false}",
            "blocked second STOP erased");
    }
    if (stage == 3) {
      Check(message.opcode == websocketpp::frame::opcode::binary &&
                Read32(message.payload, 8) == 11,
            "new input overtook retirements");
    }
    ++stage;
  }
}

/**
 * @brief 对教师提供的外部测试服务验证错误 pin、正确握手和约定的固定回复。
 *
 * 测试对端应返回“你好”、一帧含两个 sample 的短音频及 done；不是任意生产问答服务。
 * host/port/pin 从命令行取得，此路径不测试 UDP 发现，不需要板端或云服务 API Key。
 */
void ExternalServerSmoke(const char* host, const char* port, const char* pin) {
  const auto parsed_port = std::stoul(port);
  Check(parsed_port > 0 && parsed_port <= 65535, "invalid smoke port");
  VoiceClientConfig config{kDeviceId, host, static_cast<std::uint16_t>(parsed_port), pin};
  auto bad_config = config;
  bad_config.server_spki_sha256[0] = bad_config.server_spki_sha256[0] == 'A' ? 'B' : 'A';
  VoiceLink bad_link;
  Check(bad_link.Open(bad_config) && WaitEvent(bad_link).kind == LinkEventKind::Offline,
        "server accepted wrong pin");
  bad_link.Close();
  VoiceLink link;
  Check(link.Open(config) && WaitEvent(link).kind == LinkEventKind::Online,
        "server did not become ready");
  std::array<std::int16_t, 320> pcm{};
  pcm[0] = 0x0807;
  Check(link.SendAudio(1, pcm.data(), true, true, false) == SendResult::Ok,
        "smoke input rejected");
  bool have_text = false, have_audio = false, done = false;
  for (unsigned i = 0; i < 8 && !done; ++i) {
    const auto event = WaitEvent(link);
    if (event.kind == LinkEventKind::Text) {
      have_text = event.text == "你好";
    } else if (event.kind == LinkEventKind::Audio) {
      have_audio = event.start && event.end && event.audio_size == 4;
    } else if (event.kind == LinkEventKind::Done) {
      done = true;
    } else {
      throw std::runtime_error("smoke received failure");
    }
  }
  Check(have_text && have_audio && done, "server did not complete expected reply");
  std::cout << "WSS_SMOKE_OK\n";
}

}  // namespace

/// @brief 无参数运行本机回环场景；三个参数运行外部测试服务 smoke，失败统一返回非零。
int main(int argc, char** argv) {
  try {
    if (argc == 4) {
      ExternalServerSmoke(argv[1], argv[2], argv[3]);
      return EXIT_SUCCESS;
    }
    BasicWireAndClose();
    GenerationFence();
    StopAfterEnd();
    ErrorEndsUpload();
    RejectBrokenWire();
    TlsFailureAndReconnect();
    BoundedQueuesAndSupersede();
    PendingRetirementKeepsMeaning();
    RejectOldSampleRate();
    HandshakeTimeout();
  } catch (const std::exception& error) {
    std::cerr << "voice link loopback: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
  std::cout << "voice link loopback: passed\n";
  return EXIT_SUCCESS;
}
