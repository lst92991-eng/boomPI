/// @file WSS/TLS 驱动及当前 v1 wire 校验；不包含对话状态转换。
#include "boompi/network/voice_transport.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <limits>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#include <websocketpp/client.hpp>
#include <websocketpp/config/asio_client.hpp>

namespace boompi::network {
namespace {

constexpr std::size_t kHeaderBytes = 64;
constexpr std::size_t kMaxUplinkPcmBytes = 640U;
constexpr std::size_t kMaxDownlinkPcmBytes = Inbound::kMaximumPcmBytes;
constexpr std::size_t kMaxMessageBytes = kHeaderBytes + 64U * 1024U;
constexpr std::size_t kMaxBufferedBytes = 28U * 1024U;  // 约 800 ms 上行帧。
constexpr std::uint16_t kKnownFlags = 7;

void Put16(std::uint16_t v, std::uint8_t* p) {
  p[0] = static_cast<std::uint8_t>(v >> 8U); p[1] = static_cast<std::uint8_t>(v);
}
void Put32(std::uint32_t v, std::uint8_t* p) {
  for (int i = 3; i >= 0; --i) { p[i] = static_cast<std::uint8_t>(v); v >>= 8U; }
}
void Put64(std::uint64_t v, std::uint8_t* p) {
  for (int i = 7; i >= 0; --i) { p[i] = static_cast<std::uint8_t>(v); v >>= 8U; }
}
std::uint16_t Get16(const std::uint8_t* p) {
  return static_cast<std::uint16_t>((p[0] << 8U) | p[1]);
}
std::uint32_t Get32(const std::uint8_t* p) {
  std::uint32_t v = 0; for (int i = 0; i != 4; ++i) v = (v << 8U) | p[i]; return v;
}
std::uint64_t Get64(const std::uint8_t* p) {
  std::uint64_t v = 0; for (int i = 0; i != 8; ++i) v = (v << 8U) | p[i]; return v;
}
bool SameTurn(const WireIds& a, const WireIds& b) {
  return a.session == b.session && a.turn == b.turn && a.epoch == b.epoch;
}
bool SameIds(const WireIds& a, const WireIds& b) {
  return SameTurn(a, b) && a.stream == b.stream;
}
std::string Quote(const std::string& value) {
  // 控制帧仍由本模块构造，但所有字符串都经过完整 JSON escaping，禁止直接拼原值。
  std::string out(1, '"');
  constexpr char hex[] = "0123456789abcdef";
  for (const char byte : value) {
    const auto c = static_cast<unsigned char>(byte);
    if (c == '"' || c == '\\') { out.push_back('\\'); out.push_back(static_cast<char>(c)); }
    else if (c >= 0x20U) out.push_back(byte);
    else { out.append("\\u00"); out.push_back(hex[c >> 4U]); out.push_back(hex[c & 15U]); }
  }
  out.push_back('"'); return out;
}
bool ParseUuid(const std::string& value, std::array<std::uint8_t, 16>* out) {
  if (!out || value.size() != 36 || value[8] != '-' || value[13] != '-' ||
      value[18] != '-' || value[23] != '-') return false;
  std::size_t n = 0; int high = -1;
  for (char c : value) {
    if (c == '-') continue;
    const int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
    if (d < 0) return false;
    if (high < 0) high = d; else { (*out)[n++] = static_cast<std::uint8_t>((high << 4) | d); high = -1; }
  }
  return n == out->size();
}

}  // namespace

class VoiceTransport::Impl final {
  using Client = websocketpp::client<websocketpp::config::asio_tls_client>;
  using Hdl = websocketpp::connection_hdl;
  using Tls = websocketpp::lib::shared_ptr<websocketpp::lib::asio::ssl::context>;
 public:
  ~Impl() { Close(); }

  bool Connect(TransportConfig c, InboundHandler in, ErrorHandler err) {
    // UUID 与 SPKI pin 在启动线程前一次性验证；普通连接绝不降级为明文或 VERIFY_NONE。
    if (thread_.joinable() || c.host.empty() || !ParseUuid(c.device_id, &uuid_) ||
        !DecodePin(c.spki_sha256_base64) || c.port == 0) return false;
    config_ = std::move(c); inbound_ = std::move(in); error_ = std::move(err);
    client_.clear_access_channels(websocketpp::log::alevel::all);
    client_.clear_error_channels(websocketpp::log::elevel::all);
    client_.init_asio(); client_.start_perpetual();
    client_.set_tls_init_handler([this](Hdl) { return MakeTls(); });
    client_.set_open_handler([this](Hdl h) {
      hdl_ = h;
      // Close 可能与 TLS 握手完成同时发生；关闭已经开始时绝不能把连接重新标为可用。
      if (closing_.load(std::memory_order_acquire)) {
        websocketpp::lib::error_code ec;
        client_.close(h, websocketpp::close::status::normal, "", ec);
        return;
      }
      connected_.store(true, std::memory_order_release);
    });
    client_.set_fail_handler([this](Hdl h) { Fail(client_.get_con_from_hdl(h)->get_ec().message()); });
    client_.set_close_handler([this](Hdl h) {
      connected_ = false; auto c = client_.get_con_from_hdl(h);
      if (!closing_) Fail(c->get_remote_close_reason().empty() ? "connection closed" :
                                                           c->get_remote_close_reason());
    });
    client_.set_message_handler([this](Hdl, Client::message_ptr m) { OnMessage(m); });
    client_.set_ping_handler([this](Hdl, const std::string&) { Inbound e{}; e.kind = InboundKind::kHeartbeat; Emit(std::move(e)); return true; });
    websocketpp::lib::error_code ec;
    const std::string uri = "wss://" + config_.host + ":" + std::to_string(config_.port) + "/ws";
    auto con = client_.get_connection(uri, ec);
    if (ec) return false;
    con->set_open_handshake_timeout(5000);
    con->set_max_message_size(kMaxMessageBytes);
    client_.connect(con);
    try { thread_ = std::thread([this] { client_.run(); }); }
    catch (...) { client_.stop_perpetual(); return false; }
    return true;
  }

  bool Send(const Control& c) {
    // protocol_mutex 同时保护上下行序号、active turn 和 response 身份，使 callback 与 actor
    // 不会各自接受一个彼此矛盾的状态。
    std::lock_guard<std::mutex> state_lock(protocol_mutex_);
    if (c.message_id.empty() || c.message_id.size() > 64) return false;
    const char* type = nullptr; std::string payload;
    switch (c.kind) {
      case ControlKind::kHello:
        if (c.ids.session || c.ids.turn || c.ids.stream || c.ids.epoch || c.device_token.empty()) return false;
        type = "hello"; payload = "{\"device_token\":" + Quote(c.device_token) + "}"; break;
      case ControlKind::kTurnStart:
        if (!ValidIds(c.ids) || active_ ||
            c.ids.session != session_ || c.ids.epoch < session_epoch_ ||
            (turn_.epoch != 0U && c.ids.epoch <= turn_.epoch)) return false;
        type = "turn.start"; payload = "{\"sample_rate_hz\":16000}"; break;
      case ControlKind::kTurnCommit:
        if (!active_ || committed_ || !uplink_ended_ || c.ids.stream != turn_.stream || !SameTurn(c.ids, turn_)) return false;
        type = "turn.commit"; payload = "{}"; break;
      case ControlKind::kTurnCancel:
        if (!active_ || response_cancel_sent_ || !SameIds(c.ids, turn_)) return false;
        type = "turn.cancel"; payload = "{}"; break;
      case ControlKind::kResponseCancel:
        if (!SameTurn(c.ids, turn_) ||
            (c.ids.stream != turn_.stream && c.ids.stream != response_.stream)) return false;
        // ASIO 可能已解析 response.done，但 application 还没消费该事件；此时取消已自然完成。
        if (!active_) return response_done_ && ValidIds(response_) && SameIds(c.ids, response_);
        if (response_cancel_sent_) return true;
        type = "response.cancel"; payload = "{}"; break;
    }
    std::string json = "{\"version\":1,\"type\":\"" + std::string(type) +
      "\",\"message_id\":" + Quote(c.message_id) + ",\"device_id\":" + Quote(config_.device_id) +
      ",\"session_id\":" + std::to_string(c.ids.session) + ",\"turn_id\":" + std::to_string(c.ids.turn) +
      ",\"stream_id\":" + std::to_string(c.ids.stream) + ",\"epoch\":" + std::to_string(c.ids.epoch) +
      ",\"payload\":" + payload + "}";
    if (!SendText(json)) return false;
    if (c.kind == ControlKind::kTurnStart) {
      turn_ = c.ids; response_ = {}; active_ = true; committed_ = false;
      uplink_ended_ = audio_started_ = audio_ended_ = response_done_ = false;
      response_cancel_sent_ = discard_downlink_ = false; uplink_sequence_ = downlink_sequence_ = 0;
    }
    if (c.kind == ControlKind::kTurnCommit) committed_ = true;
    if (c.kind == ControlKind::kTurnCancel || c.kind == ControlKind::kResponseCancel)
      response_cancel_sent_ = true;
    return true;
  }

  bool SendText(const std::string& json) {
    if (!connected_ || json.empty() || json.size() > 64U * 1024U) return false;
    websocketpp::lib::error_code ec; std::lock_guard<std::mutex> lock(send_mutex_);
    if (!CanQueue(json.size())) return false;
    client_.send(hdl_, json, websocketpp::frame::opcode::text, ec); return !ec;
  }

  SendOutcome SendPcm(const Pcm64Header& h, const std::uint8_t* pcm, std::size_t bytes) {
    std::lock_guard<std::mutex> state_lock(protocol_mutex_);
    if (!connected_ || !pcm || bytes == 0 || bytes > kMaxUplinkPcmBytes || bytes % 2 ||
        h.flags & ~kKnownFlags || !active_ || committed_ || uplink_ended_ ||
        !SameTurn(h.ids, turn_) || h.ids.stream != turn_.stream || h.sequence != uplink_sequence_ ||
        h.sequence == std::numeric_limits<std::uint32_t>::max() || ((h.sequence == 0) != ((h.flags & 1U) != 0)))
      return SendOutcome::kRejected;
    // 字段逐个按网络字节序写入，不能直接发送 C++ struct（padding/endianness 不稳定）。
    std::array<std::uint8_t, kHeaderBytes + kMaxUplinkPcmBytes> frame; auto* p = frame.data();
    std::memcpy(p, "BPV1", 4); p[4] = 1; p[5] = 1; Put16(h.flags, p + 6); Put16(64, p + 8);
    p[10] = 1; p[11] = 1; Put32(16000U, p + 12); Put32(static_cast<std::uint32_t>(bytes), p + 16);
    Put32(h.sequence, p + 20); Put64(h.timestamp_us, p + 24); Put32(h.ids.epoch, p + 32);
    std::memcpy(p + 36, uuid_.data(), uuid_.size()); Put32(h.ids.session, p + 52);
    Put32(h.ids.turn, p + 56); Put32(h.ids.stream, p + 60); std::memcpy(p + 64, pcm, bytes);
    websocketpp::lib::error_code ec; std::lock_guard<std::mutex> lock(send_mutex_);
    // 缓冲满只是瞬时背压：本帧未发、sequence 不前移，由调用方取消本轮。
    if (!CanQueue(kHeaderBytes + bytes)) return SendOutcome::kBackpressure;
    client_.send(hdl_, frame.data(), kHeaderBytes + bytes, websocketpp::frame::opcode::binary, ec);
    if (ec) return SendOutcome::kRejected;
    ++uplink_sequence_; uplink_ended_ = (h.flags & 2U) != 0;
    return SendOutcome::kOk;
  }

  void Close() noexcept {
    // closing_ 保证多条失败/信号路径只发一次 close；stop 还会取消尚未完成的 DNS/TCP/TLS，
    // 避免 connected=false 时只停 perpetual work、却在 join 中永远等待 pending connect。
    if (!closing_.exchange(true)) {
      websocketpp::lib::error_code ec;
      if (connected_.exchange(false)) client_.close(hdl_, websocketpp::close::status::normal, "", ec);
      client_.stop_perpetual();
      client_.stop();
    }
    if (thread_.joinable() && thread_.get_id() != std::this_thread::get_id()) thread_.join();
  }
  bool connected() const noexcept { return connected_; }

 private:
  static bool ValidIds(const WireIds& i) { return i.session && i.turn && i.stream && i.epoch; }
  bool IsStale(const WireIds& i) const {
    if (i.session != session_ || i.epoch == 0U) return false;
    if (turn_.epoch != 0U && i.epoch < turn_.epoch) return true;
    return ValidIds(response_) && SameIds(i, response_) && !active_;
  }
  bool Discarding(const WireIds& i) const {
    return ValidIds(response_) && SameIds(i, response_) &&
        (response_cancel_sent_ || discard_downlink_);
  }
  bool CanQueue(std::size_t bytes) {
    try {
      const std::size_t queued = client_.get_con_from_hdl(hdl_)->get_buffered_amount();
      return queued <= kMaxBufferedBytes && bytes <= kMaxBufferedBytes - queued;
    } catch (...) { return false; }
  }
  void Fail(std::string why) { connected_ = false; if (error_) try { error_(std::move(why)); } catch (...) {} }
  void Emit(Inbound event) { if (inbound_) try { inbound_(std::move(event)); } catch (...) { Fail("inbound callback threw"); } }

  bool DecodePin(const std::string& text) {
    if (text.size() != 44 || text.back() != '=') return false;
    std::array<unsigned char, 33> decoded{};
    const int n = EVP_DecodeBlock(decoded.data(), reinterpret_cast<const unsigned char*>(text.data()), 44);
    if (n != 33) return false;
    std::copy_n(decoded.begin(), pin_.size(), pin_.begin()); return true;
  }
  static int VerifyPin(X509_STORE_CTX* store, void* arg) {
    // 固定的是证书公钥 SPKI SHA-256，不是整张证书；同一密钥续签不会导致设备失配。
    auto* self = static_cast<Impl*>(arg); X509* cert = X509_STORE_CTX_get0_cert(store);
    if (!self || !cert || X509_check_purpose(cert, X509_PURPOSE_SSL_SERVER, 0) <= 0) return 0;
    X509_PUBKEY* key = X509_get_X509_PUBKEY(cert); int n = i2d_X509_PUBKEY(key, nullptr);
    if (n <= 0) return 0;
    std::vector<unsigned char> der(static_cast<std::size_t>(n)); unsigned char* cursor = der.data();
    std::array<unsigned char, 32> digest{}; unsigned int size = 0;
    if (i2d_X509_PUBKEY(key, &cursor) != n || EVP_Digest(der.data(), der.size(), digest.data(), &size, EVP_sha256(), nullptr) != 1 ||
        size != digest.size() || CRYPTO_memcmp(digest.data(), self->pin_.data(), digest.size()) != 0) return 0;
    X509_STORE_CTX_set_error(store, X509_V_OK); return 1;
  }
  Tls MakeTls() {
    auto ctx = websocketpp::lib::make_shared<websocketpp::lib::asio::ssl::context>(websocketpp::lib::asio::ssl::context::tls_client);
    SSL_CTX_set_min_proto_version(ctx->native_handle(), TLS1_2_VERSION);
    SSL_CTX_set_options(ctx->native_handle(), SSL_OP_NO_COMPRESSION);
    SSL_CTX_set_verify(ctx->native_handle(), SSL_VERIFY_PEER, nullptr);
    SSL_CTX_set_cert_verify_callback(ctx->native_handle(), &VerifyPin, this); return ctx;
  }

  bool Identity(const boost::property_tree::ptree& p, Inbound* event) {
    try {
      if (p.get<unsigned>("version") != 1 || p.get<std::string>("device_id") != config_.device_id) return false;
      event->ids = {p.get<std::uint32_t>("session_id"), p.get<std::uint32_t>("turn_id"),
                    p.get<std::uint32_t>("stream_id"), p.get<std::uint32_t>("epoch")};
      const std::string id = p.get<std::string>("message_id"); return !id.empty() && id.size() <= 64;
    } catch (...) { return false; }
  }
  void OnText(const std::string& json) {
    // 解析、字段上限、identity 和状态校验全部在发出 Inbound 之前完成；半合法事件不外泄。
    if (json.empty() || json.size() > 64U * 1024U) return ProtocolError("invalid text message size");
    try {
      boost::property_tree::ptree p; std::istringstream input(json); boost::property_tree::read_json(input, p);
      Inbound e;
      {
        std::lock_guard<std::mutex> state_lock(protocol_mutex_);
        if (!Identity(p, &e)) throw std::runtime_error("invalid control envelope");
        const auto& body = p.get_child("payload"); const std::string type = p.get<std::string>("type");
        if (type == "hello.ack") {
          if (session_ || e.ids.session == 0 || e.ids.turn || e.ids.stream || e.ids.epoch == 0) throw std::runtime_error("invalid hello.ack identity");
          e.kind = InboundKind::kHelloAck;
          if (body.get<std::uint32_t>("input_sample_rate_hz") != 16000 ||
              body.get<std::uint32_t>("output_sample_rate_hz") != 24000 ||
              body.get<std::uint32_t>("input_frame_ms") != 20) throw std::runtime_error("unsupported hello audio contract");
          session_ = e.ids.session; session_epoch_ = e.ids.epoch;
        } else if (type == "response.start") {
          if (IsStale(e.ids)) return;
          if (!active_ || !committed_ || ValidIds(response_) ||
              !SameTurn(e.ids, turn_) || e.ids.stream == 0) throw std::runtime_error("wrong response.start identity or order");
          e.kind = InboundKind::kResponseStart;
          if (body.get<std::string>("response_id").empty()) throw std::runtime_error("missing response_id");
          response_ = e.ids; downlink_sequence_ = 0;
          audio_started_ = audio_ended_ = response_done_ = false;
          if (response_cancel_sent_) { discard_downlink_ = true; return; }
          discard_downlink_ = false;
        } else if (type == "response.cancelled") {
          if (IsStale(e.ids)) return;
          const bool stream_matches = ValidIds(response_) ? e.ids.stream == response_.stream :
                                                            e.ids.stream != 0U;
          if (!active_ || !response_cancel_sent_ || !SameTurn(e.ids, turn_) || !stream_matches)
            throw std::runtime_error("wrong cancellation identity");
          e.kind = InboundKind::kCancelled; const std::string reason = body.get<std::string>("reason");
          if (reason.empty() || reason.size() > 64) throw std::runtime_error("invalid cancellation reason");
          active_ = response_cancel_sent_ = discard_downlink_ = false;
        } else if (type == "error") {
          if (IsStale(e.ids)) return;
          e.kind = InboundKind::kError; e.error_code = body.get<std::string>("code"); e.text = body.get<std::string>("message");
          if (e.error_code.empty() || e.error_code.size() > 64 || e.text.empty() || e.text.size() > 512) throw std::runtime_error("invalid error payload");
          const bool session_error = e.ids.session == session_ && e.ids.turn == 0U &&
              e.ids.stream == 0U && e.ids.epoch == session_epoch_;
          const bool turn_error = active_ && SameTurn(e.ids, turn_) && e.ids.stream != 0U;
          if (!session_error && !turn_error) throw std::runtime_error("wrong error identity");
          if (turn_error) active_ = false;
        } else {
          if (IsStale(e.ids)) return;
          if (!active_ || !ValidIds(response_) || e.ids.session != response_.session || e.ids.turn != response_.turn ||
              e.ids.stream != response_.stream || e.ids.epoch != response_.epoch) throw std::runtime_error("wrong response identity");
          if (type == "response.text_delta") {
            if (Discarding(e.ids)) return;
            e.kind = InboundKind::kTextDelta; e.text = body.get<std::string>("text");
            if (e.text.empty() || e.text.size() > 4096) throw std::runtime_error("invalid text delta");
          } else if (type == "response.audio_start") {
            if (Discarding(e.ids)) return;
            e.kind = InboundKind::kAudioStart;
            if (audio_started_ || body.get<std::uint32_t>("sample_rate_hz") != 24000) {
              throw std::runtime_error("invalid audio contract");
            }
            audio_started_ = true;
          } else if (type == "response.done") {
            e.kind = InboundKind::kDone;
            if (audio_started_ && !audio_ended_ && !Discarding(e.ids)) throw std::runtime_error("done before PCM END");
            active_ = response_cancel_sent_ = discard_downlink_ = false; response_done_ = true;
          }
          else throw std::runtime_error("unsupported control type");
        }
      }
      Emit(std::move(e));
    } catch (const std::exception& e) { ProtocolError(e.what()); }
  }
  void OnBinary(const std::string& bytes) {
    // 下行 PCM 必须连续递增且匹配当前 response 四元组；不尝试补发或跳过中间音频。
    const auto* p = reinterpret_cast<const std::uint8_t*>(bytes.data());
    if (bytes.size() < kHeaderBytes || std::memcmp(p, "BPV1", 4) != 0 || p[4] != 1 || p[5] != 2 ||
        Get16(p + 8) != 64 || p[10] != 1 || p[11] != 1) return ProtocolError("invalid PCM header");
    const std::size_t payload = Get32(p + 16); const std::uint16_t flags = Get16(p + 6);
    Inbound e; e.kind = InboundKind::kPcm; e.pcm.flags = flags;
    e.pcm.sequence = Get32(p + 20); e.pcm.timestamp_us = Get64(p + 24);
    e.pcm.ids = {Get32(p + 52), Get32(p + 56), Get32(p + 60), Get32(p + 32)}; e.ids = e.pcm.ids;
    if (payload == 0 || payload > kMaxDownlinkPcmBytes || payload % 2 ||
        bytes.size() != kHeaderBytes + payload || flags & ~kKnownFlags ||
        Get32(p + 12) != 24000 || std::memcmp(p + 36, uuid_.data(), uuid_.size()) != 0)
      return ProtocolError("invalid PCM framing");
    {
      std::unique_lock<std::mutex> state_lock(protocol_mutex_);
      if (IsStale(e.ids) || Discarding(e.ids)) return;
      if (!audio_started_ || audio_ended_ ||
          e.pcm.sequence != downlink_sequence_ || downlink_sequence_ == std::numeric_limits<std::uint32_t>::max() ||
          e.pcm.ids.session != response_.session || e.pcm.ids.turn != response_.turn ||
          e.pcm.ids.stream != response_.stream || e.pcm.ids.epoch != response_.epoch ||
          ((downlink_sequence_ == 0) != ((flags & 1U) != 0))) { state_lock.unlock(); return ProtocolError("PCM identity or sequence mismatch"); }
      if ((flags & 4U) != 0U) {
        discard_downlink_ = true; e.kind = InboundKind::kError;
        e.error_code = "downlink_discontinuity"; e.text = "downlink PCM is discontinuous";
      } else {
        ++downlink_sequence_; audio_ended_ = (flags & 2U) != 0;
      }
    }
    if (e.kind == InboundKind::kError) { Emit(std::move(e)); return; }
    std::copy_n(p + kHeaderBytes, payload, e.audio.begin()); e.audio_size = payload; Emit(std::move(e));
  }
  void OnMessage(const Client::message_ptr& message) {
    if (message->get_payload().size() > kMaxMessageBytes) return ProtocolError("message too large");
    if (message->get_opcode() == websocketpp::frame::opcode::text) OnText(message->get_payload());
    else if (message->get_opcode() == websocketpp::frame::opcode::binary) OnBinary(message->get_payload());
    else ProtocolError("unsupported WebSocket opcode");
  }
  void ProtocolError(const std::string& why) {
    Fail(why); websocketpp::lib::error_code ec;
    client_.close(hdl_, websocketpp::close::status::policy_violation, "protocol error", ec);
  }

  // ASIO 线程拥有 client_/hdl_ 回调；protocol_mutex_ 是它与 application actor 的唯一协议共享边界。
  Client client_; Hdl hdl_; TransportConfig config_; InboundHandler inbound_; ErrorHandler error_;
  std::thread thread_; std::mutex send_mutex_, protocol_mutex_;
  std::atomic<bool> connected_{false}, closing_{false};
  std::array<std::uint8_t, 16> uuid_{}; std::array<unsigned char, 32> pin_{};
  WireIds turn_{}, response_{}; std::uint32_t session_{0}, session_epoch_{0};
  std::uint32_t uplink_sequence_{0}, downlink_sequence_{0};
  bool active_{false}, committed_{false}, uplink_ended_{false}, audio_started_{false}, audio_ended_{false};
  bool response_done_{false}, response_cancel_sent_{false}, discard_downlink_{false};
};

VoiceTransport::VoiceTransport() : impl_(std::make_unique<Impl>()) {}
VoiceTransport::~VoiceTransport() = default;
bool VoiceTransport::Connect(TransportConfig c, InboundHandler i, ErrorHandler e) { return impl_->Connect(std::move(c), std::move(i), std::move(e)); }
bool VoiceTransport::SendControl(const Control& c) { return impl_->Send(c); }
SendOutcome VoiceTransport::SendPcm64(const Pcm64Header& h, const std::uint8_t* p, std::size_t n) { return impl_->SendPcm(h, p, n); }
void VoiceTransport::Close() noexcept { impl_->Close(); }
bool VoiceTransport::connected() const noexcept { return impl_->connected(); }

}  // namespace boompi::network
