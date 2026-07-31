#include "boompi/voice/transport.h"

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

namespace boompi::voice {
namespace {

constexpr std::size_t kHeaderBytes = 64;
constexpr std::size_t kMaxPcmBytes = 64U * 1024U;
constexpr std::size_t kMaxUplinkPcmBytes = 640U;
constexpr std::size_t kMaxDownlinkPcmBytes = Inbound::kMaximumPcmBytes;
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
std::string Quote(const std::string& value) {
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

class Transport::Impl final {
  using Client = websocketpp::client<websocketpp::config::asio_tls_client>;
  using Hdl = websocketpp::connection_hdl;
  using Tls = websocketpp::lib::shared_ptr<websocketpp::lib::asio::ssl::context>;
 public:
  ~Impl() { Close(); }

  bool Connect(TransportConfig c, InboundHandler in, ErrorHandler err, CloseHandler close) {
    if (thread_.joinable() || c.host.empty() || !ParseUuid(c.device_id, &uuid_) ||
        !DecodePin(c.spki_sha256_base64) || c.port == 0 || c.connect_timeout_ms == 0 ||
        c.maximum_message_bytes < kHeaderBytes || c.maximum_message_bytes > kHeaderBytes + kMaxPcmBytes ||
        c.maximum_buffered_bytes < c.maximum_message_bytes) return false;
    config_ = std::move(c); inbound_ = std::move(in); error_ = std::move(err); closed_ = std::move(close);
    client_.clear_access_channels(websocketpp::log::alevel::all);
    client_.clear_error_channels(websocketpp::log::elevel::all);
    client_.init_asio(); client_.start_perpetual();
    client_.set_tls_init_handler([this](Hdl) { return MakeTls(); });
    client_.set_open_handler([this](Hdl h) { hdl_ = h; connected_ = true; });
    client_.set_fail_handler([this](Hdl h) { Fail(client_.get_con_from_hdl(h)->get_ec().message()); });
    client_.set_close_handler([this](Hdl h) {
      connected_ = false; auto c = client_.get_con_from_hdl(h);
      SafeClose(static_cast<std::uint16_t>(c->get_remote_close_code()), c->get_remote_close_reason());
    });
    client_.set_message_handler([this](Hdl, Client::message_ptr m) { OnMessage(m); });
    client_.set_ping_handler([](Hdl, const std::string&) { return true; });
    websocketpp::lib::error_code ec;
    const std::string uri = "wss://" + config_.host + ":" + std::to_string(config_.port) + "/ws";
    auto con = client_.get_connection(uri, ec);
    if (ec) return false;
    con->set_open_handshake_timeout(config_.connect_timeout_ms);
    con->set_max_message_size(config_.maximum_message_bytes);
    client_.connect(con);
    try { thread_ = std::thread([this] { client_.run(); }); }
    catch (...) { client_.stop_perpetual(); return false; }
    return true;
  }

  bool Send(const Control& c) {
    std::lock_guard<std::mutex> state_lock(protocol_mutex_);
    if (c.message_id.empty() || c.message_id.size() > 64) return false;
    const char* type = nullptr; std::string payload;
    switch (c.kind) {
      case ControlKind::kHello:
        if (c.ids.session || c.ids.turn || c.ids.stream || c.ids.epoch || c.device_token.empty()) return false;
        type = "hello"; payload = "{\"device_token\":" + Quote(c.device_token) + "}"; break;
      case ControlKind::kTurnStart:
        if (!ValidIds(c.ids) || c.sample_rate_hz != 16000 || active_ ||
            c.ids.session != session_ || c.ids.epoch < session_epoch_) return false;
        type = "turn.start"; payload = "{\"sample_rate_hz\":16000}"; break;
      case ControlKind::kTurnCommit:
        if (!active_ || committed_ || !uplink_ended_ || c.ids.stream != turn_.stream || !SameTurn(c.ids, turn_)) return false;
        type = "turn.commit"; payload = "{}"; break;
      case ControlKind::kResponseCancel:
        if (!active_ || !SameTurn(c.ids, turn_) ||
            (c.ids.stream != turn_.stream && c.ids.stream != response_.stream)) return false;
        type = "response.cancel"; payload = "{}"; break;
    }
    std::string json = "{\"version\":1,\"type\":\"" + std::string(type) +
      "\",\"message_id\":" + Quote(c.message_id) + ",\"device_id\":" + Quote(config_.device_id) +
      ",\"session_id\":" + std::to_string(c.ids.session) + ",\"turn_id\":" + std::to_string(c.ids.turn) +
      ",\"stream_id\":" + std::to_string(c.ids.stream) + ",\"epoch\":" + std::to_string(c.ids.epoch) +
      ",\"payload\":" + payload + "}";
    if (!SendText(json)) return false;
    if (c.kind == ControlKind::kTurnStart) { turn_ = c.ids; active_ = true; committed_ = false; uplink_ended_ = false; uplink_sequence_ = 0; }
    if (c.kind == ControlKind::kTurnCommit) committed_ = true;
    return true;
  }

  bool SendText(const std::string& json) {
    if (!connected_ || json.empty() || json.size() > 64U * 1024U) return false;
    websocketpp::lib::error_code ec; std::lock_guard<std::mutex> lock(send_mutex_);
    if (!CanQueue(json.size())) return false;
    client_.send(hdl_, json, websocketpp::frame::opcode::text, ec); return !ec;
  }

  bool SendPcm(const Pcm64Header& h, const std::uint8_t* pcm, std::size_t bytes) {
    std::lock_guard<std::mutex> state_lock(protocol_mutex_);
    if (!connected_ || !pcm || bytes == 0 || bytes > kMaxUplinkPcmBytes || bytes % 2 || h.kind != 1 ||
        h.flags & ~kKnownFlags || h.sample_rate_hz != 16000 || !active_ || committed_ || uplink_ended_ ||
        !SameTurn(h.ids, turn_) || h.ids.stream != turn_.stream || h.sequence != uplink_sequence_ ||
        h.sequence == std::numeric_limits<std::uint32_t>::max() || ((h.sequence == 0) != ((h.flags & 1U) != 0))) return false;
    std::array<std::uint8_t, kHeaderBytes + kMaxUplinkPcmBytes> frame; auto* p = frame.data();
    std::memcpy(p, "BPV1", 4); p[4] = 1; p[5] = 1; Put16(h.flags, p + 6); Put16(64, p + 8);
    p[10] = 1; p[11] = 1; Put32(h.sample_rate_hz, p + 12); Put32(static_cast<std::uint32_t>(bytes), p + 16);
    Put32(h.sequence, p + 20); Put64(h.timestamp_us, p + 24); Put32(h.ids.epoch, p + 32);
    std::memcpy(p + 36, uuid_.data(), uuid_.size()); Put32(h.ids.session, p + 52);
    Put32(h.ids.turn, p + 56); Put32(h.ids.stream, p + 60); std::memcpy(p + 64, pcm, bytes);
    websocketpp::lib::error_code ec; std::lock_guard<std::mutex> lock(send_mutex_);
    if (!CanQueue(kHeaderBytes + bytes)) return false;
    client_.send(hdl_, frame.data(), kHeaderBytes + bytes, websocketpp::frame::opcode::binary, ec);
    if (!ec) { ++uplink_sequence_; uplink_ended_ = (h.flags & 2U) != 0; }
    return !ec;
  }

  void Close() noexcept {
    if (!closing_.exchange(true)) {
      websocketpp::lib::error_code ec;
      if (connected_.exchange(false)) client_.close(hdl_, websocketpp::close::status::normal, "", ec);
      client_.stop_perpetual();
    }
    if (thread_.joinable() && thread_.get_id() != std::this_thread::get_id()) thread_.join();
  }
  bool connected() const noexcept { return connected_; }

 private:
  static bool ValidIds(const WireIds& i) { return i.session && i.turn && i.stream && i.epoch; }
  bool CanQueue(std::size_t bytes) {
    try {
      const std::size_t queued = client_.get_con_from_hdl(hdl_)->get_buffered_amount();
      return queued <= config_.maximum_buffered_bytes && bytes <= config_.maximum_buffered_bytes - queued;
    } catch (...) { return false; }
  }
  void Fail(std::string why) { connected_ = false; if (error_) try { error_(std::move(why)); } catch (...) {} }
  void SafeClose(std::uint16_t code, std::string why) { if (closed_) try { closed_(code, std::move(why)); } catch (...) {} }
  void Emit(Inbound event) { if (inbound_) try { inbound_(std::move(event)); } catch (...) { Fail("inbound callback threw"); } }

  bool DecodePin(const std::string& text) {
    if (text.size() != 44 || text.back() != '=') return false;
    std::array<unsigned char, 33> decoded{};
    const int n = EVP_DecodeBlock(decoded.data(), reinterpret_cast<const unsigned char*>(text.data()), 44);
    if (n != 33) return false;
    std::copy_n(decoded.begin(), pin_.size(), pin_.begin()); return true;
  }
  static int VerifyPin(X509_STORE_CTX* store, void* arg) {
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
          e.kind = InboundKind::kHelloAck; e.input_sample_rate_hz = body.get<std::uint32_t>("input_sample_rate_hz");
          e.output_sample_rate_hz = body.get<std::uint32_t>("output_sample_rate_hz"); e.input_frame_ms = body.get<std::uint32_t>("input_frame_ms");
          if (e.input_sample_rate_hz != 16000 || e.output_sample_rate_hz != 24000 || e.input_frame_ms != 20) throw std::runtime_error("unsupported hello audio contract");
          session_ = e.ids.session; session_epoch_ = e.ids.epoch;
        } else if (type == "response.start") {
          if (!active_ || !SameTurn(e.ids, turn_) || e.ids.stream == 0) throw std::runtime_error("wrong response.start identity");
          e.kind = InboundKind::kResponseStart; e.response_id = body.get<std::string>("response_id");
          if (e.response_id.empty() || e.response_id.size() > 128) throw std::runtime_error("invalid response_id");
          response_ = e.ids; response_id_ = e.response_id; downlink_sequence_ = 0; audio_started_ = false; audio_ended_ = false;
        } else if (type == "response.cancelled") {
          const std::uint32_t stream = ValidIds(response_) ? response_.stream : turn_.stream;
          if (!active_ || !SameTurn(e.ids, turn_) || e.ids.stream != stream) throw std::runtime_error("wrong cancellation identity");
          e.kind = InboundKind::kCancelled; e.reason = body.get<std::string>("reason");
          if (e.reason.empty() || e.reason.size() > 64) throw std::runtime_error("invalid cancellation reason");
          active_ = false;
        } else if (type == "error") {
          e.kind = InboundKind::kError; e.error_code = body.get<std::string>("code"); e.text = body.get<std::string>("message");
          if (e.error_code.empty() || e.error_code.size() > 64 || e.text.empty() || e.text.size() > 512) throw std::runtime_error("invalid error payload");
          if (SameTurn(e.ids, turn_)) active_ = false;
        } else {
          if (!active_ || !ValidIds(response_) || e.ids.session != response_.session || e.ids.turn != response_.turn ||
              e.ids.stream != response_.stream || e.ids.epoch != response_.epoch) throw std::runtime_error("wrong response identity");
          e.response_id = body.get<std::string>("response_id");
          if (e.response_id != response_id_) throw std::runtime_error("response_id changed");
          if (type == "response.text_delta") { e.kind = InboundKind::kTextDelta; e.text = body.get<std::string>("text"); if (e.text.empty() || e.text.size() > 4096) throw std::runtime_error("invalid text delta"); }
          else if (type == "response.audio_start") { e.kind = InboundKind::kAudioStart; e.output_sample_rate_hz = body.get<std::uint32_t>("sample_rate_hz"); if (audio_started_ || e.output_sample_rate_hz != 24000) throw std::runtime_error("invalid audio contract"); audio_started_ = true; }
          else if (type == "response.done") { e.kind = InboundKind::kDone; if (audio_started_ && !audio_ended_) throw std::runtime_error("done before PCM END"); active_ = false; }
          else throw std::runtime_error("unsupported control type");
        }
      }
      Emit(std::move(e));
    } catch (const std::exception& e) { ProtocolError(e.what()); }
  }
  void OnBinary(const std::string& bytes) {
    const auto* p = reinterpret_cast<const std::uint8_t*>(bytes.data());
    if (bytes.size() < kHeaderBytes || std::memcmp(p, "BPV1", 4) != 0 || p[4] != 1 || p[5] != 2 ||
        Get16(p + 8) != 64 || p[10] != 1 || p[11] != 1) return ProtocolError("invalid PCM header");
    const std::size_t payload = Get32(p + 16); const std::uint16_t flags = Get16(p + 6);
    Inbound e; e.kind = InboundKind::kPcm; e.pcm.kind = p[5]; e.pcm.flags = flags; e.pcm.sample_rate_hz = Get32(p + 12);
    {
      std::unique_lock<std::mutex> state_lock(protocol_mutex_);
      if (!audio_started_ || audio_ended_ || payload == 0 || payload > kMaxDownlinkPcmBytes || payload % 2 ||
          bytes.size() != kHeaderBytes + payload || flags & ~kKnownFlags) { state_lock.unlock(); return ProtocolError("invalid PCM framing"); }
      e.pcm.sequence = Get32(p + 20); e.pcm.timestamp_us = Get64(p + 24); e.pcm.ids = {Get32(p + 52), Get32(p + 56), Get32(p + 60), Get32(p + 32)};
      if (e.pcm.sample_rate_hz != 24000 || std::memcmp(p + 36, uuid_.data(), uuid_.size()) != 0 ||
          e.pcm.sequence != downlink_sequence_ || downlink_sequence_ == std::numeric_limits<std::uint32_t>::max() ||
          e.pcm.ids.session != response_.session || e.pcm.ids.turn != response_.turn ||
          e.pcm.ids.stream != response_.stream || e.pcm.ids.epoch != response_.epoch ||
          ((downlink_sequence_ == 0) != ((flags & 1U) != 0))) { state_lock.unlock(); return ProtocolError("PCM identity or sequence mismatch"); }
      ++downlink_sequence_; audio_ended_ = (flags & 2U) != 0; e.ids = e.pcm.ids;
    }
    std::copy_n(p + kHeaderBytes, payload, e.audio.begin()); e.audio_size = payload; Emit(std::move(e));
  }
  void OnMessage(const Client::message_ptr& message) {
    if (message->get_payload().size() > config_.maximum_message_bytes) return ProtocolError("message too large");
    if (message->get_opcode() == websocketpp::frame::opcode::text) OnText(message->get_payload());
    else if (message->get_opcode() == websocketpp::frame::opcode::binary) OnBinary(message->get_payload());
    else ProtocolError("unsupported WebSocket opcode");
  }
  void ProtocolError(const std::string& why) {
    Fail(why); websocketpp::lib::error_code ec;
    client_.close(hdl_, websocketpp::close::status::policy_violation, "protocol error", ec);
  }

  Client client_; Hdl hdl_; TransportConfig config_; InboundHandler inbound_; ErrorHandler error_; CloseHandler closed_;
  std::thread thread_; std::mutex send_mutex_, protocol_mutex_;
  std::atomic<bool> connected_{false}, closing_{false};
  std::array<std::uint8_t, 16> uuid_{}; std::array<unsigned char, 32> pin_{};
  WireIds turn_{}, response_{}; std::string response_id_; std::uint32_t session_{0}, session_epoch_{0};
  std::uint32_t uplink_sequence_{0}, downlink_sequence_{0};
  bool active_{false}, committed_{false}, uplink_ended_{false}, audio_started_{false}, audio_ended_{false};
};

Transport::Transport() : impl_(std::make_unique<Impl>()) {}
Transport::~Transport() = default;
bool Transport::Connect(TransportConfig c, InboundHandler i, ErrorHandler e, CloseHandler x) { return impl_->Connect(std::move(c), std::move(i), std::move(e), std::move(x)); }
bool Transport::SendControl(const Control& c) { return impl_->Send(c); }
bool Transport::SendPcm64(const Pcm64Header& h, const std::uint8_t* p, std::size_t n) { return impl_->SendPcm(h, p, n); }
void Transport::Close() noexcept { impl_->Close(); }
bool Transport::connected() const noexcept { return impl_->connected(); }

}  // namespace boompi::voice
