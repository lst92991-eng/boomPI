/// @file WSS/TLS 驱动及当前 v1 wire 校验；不包含对话状态转换。
#include "boompi/network/voice_transport.h"

#include "boompi/config/voice_client_config.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

#include <cjson/cJSON.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#include <websocketpp/client.hpp>
#include <websocketpp/config/asio_client.hpp>
#include <websocketpp/utf8_validator.hpp>

namespace boompi::network {
namespace {

constexpr std::size_t kHeaderBytes = 64;
constexpr std::size_t kMaxUplinkPcmBytes = 640U;
constexpr std::size_t kMaxDownlinkPcmBytes = Inbound::kMaximumPcmBytes;
constexpr std::size_t kMaxMessageBytes = kHeaderBytes + 64U * 1024U;
constexpr std::size_t kMaxBufferedBytes = 28U * 1024U;  // 约 800 ms 上行帧。
constexpr auto kSendDispatchWait = std::chrono::milliseconds(20);
constexpr std::uint16_t kKnownFlags = 7;
constexpr char kTeachingDeviceToken[] =
    "boompi-teaching-shared-token-v1-2026";

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
  if (!out || !boompi::config::IsValidDeviceId(value)) return false;
  std::size_t n = 0; int high = -1;
  for (char c : value) {
    if (c == '-') continue;
    const int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
    if (d < 0) return false;
    if (high < 0) high = d; else { (*out)[n++] = static_cast<std::uint8_t>((high << 4) | d); high = -1; }
  }
  return n == out->size();
}

struct CJsonDelete final {
  void operator()(cJSON* value) const noexcept { cJSON_Delete(value); }
};

using JsonRoot = std::unique_ptr<cJSON, CJsonDelete>;

struct ParsedEnvelope final {
  JsonRoot root;
  const cJSON* payload{nullptr};
  std::string type;
  std::string device_id;
  WireIds ids{};
};

enum class SendStatus : std::uint8_t {
  kSent,
  kFull,
  kUnavailable,
};

[[noreturn]] void InvalidJson(const char* why) {
  throw std::runtime_error(why);
}

const cJSON* RequireItem(const cJSON* object, const char* name) {
  const cJSON* item = cJSON_GetObjectItemCaseSensitive(object, name);
  if (!item) InvalidJson("missing JSON field");
  return item;
}

std::string RequireString(const cJSON* object, const char* name,
                          std::size_t min_bytes, std::size_t max_bytes) {
  const cJSON* item = RequireItem(object, name);
  if (!cJSON_IsString(item) || !item->valuestring) InvalidJson("JSON field must be a string");
  std::string value(item->valuestring);
  if (value.size() < min_bytes || value.size() > max_bytes) {
    InvalidJson("JSON string is outside its byte limit");
  }
  return value;
}

std::uint32_t RequireUint32(const cJSON* object, const char* name) {
  const cJSON* item = RequireItem(object, name);
  if (!cJSON_IsNumber(item) || !std::isfinite(item->valuedouble) || item->valuedouble < 0.0 ||
      item->valuedouble > static_cast<double>(std::numeric_limits<std::uint32_t>::max()) ||
      std::trunc(item->valuedouble) != item->valuedouble) {
    InvalidJson("JSON field must be an unsigned 32-bit integer");
  }
  return static_cast<std::uint32_t>(item->valuedouble);
}

bool VisibleAscii(const std::string& value) {
  return std::all_of(value.begin(), value.end(), [](unsigned char byte) {
    return byte >= 0x21U && byte <= 0x7eU;
  });
}

void RejectNonIntegerNumberSyntax(const std::string& json) {
  bool in_string = false, escaped = false, in_number = false;
  for (const char byte : json) {
    if (in_string) {
      if (escaped) escaped = false;
      else if (byte == '\\') escaped = true;
      else if (byte == '"') in_string = false;
      continue;
    }
    if (byte == '"') { in_string = true; in_number = false; continue; }
    if (byte == '-') InvalidJson("JSON number must use unsigned integer syntax");
    if (in_number && (byte == '.' || byte == 'e' || byte == 'E'))
      InvalidJson("JSON number must use unsigned integer syntax");
    if (byte >= '0' && byte <= '9') in_number = true;
    else in_number = false;
  }
}

void RejectEscapedNul(const std::string& json) {
  bool in_string = false;
  for (std::size_t index = 0; index < json.size(); ++index) {
    const char byte = json[index];
    if (!in_string) {
      if (byte == '"') in_string = true;
      continue;
    }
    if (byte == '"') {
      in_string = false;
      continue;
    }
    if (byte != '\\' || ++index >= json.size()) continue;
    if (json[index] != 'u' || index + 4U >= json.size()) continue;
    if (json[index + 1U] == '0' && json[index + 2U] == '0' &&
        json[index + 3U] == '0' && json[index + 4U] == '0') {
      // cJSON 的字符串 API 以 NUL 结尾；必须在解码前拒绝，避免 key/value 被截断。
      InvalidJson("escaped NUL is not allowed in JSON strings");
    }
    index += 4U;
  }
}

ParsedEnvelope ParseEnvelope(const std::string& json) {
  if (json.empty() || json.size() > 64U * 1024U || !websocketpp::utf8_validator::validate(json))
    InvalidJson("invalid text message");
  RejectEscapedNul(json);
  const char* parse_end = nullptr;
  ParsedEnvelope parsed;
  parsed.root.reset(cJSON_ParseWithLengthOpts(json.c_str(), json.size() + 1U, &parse_end, true));
  if (!parsed.root || parse_end != json.c_str() + json.size()) InvalidJson("invalid JSON text");
  RejectNonIntegerNumberSyntax(json);
  if (!cJSON_IsObject(parsed.root.get())) InvalidJson("JSON envelope must be an object");
  if (RequireUint32(parsed.root.get(), "version") != 1U) InvalidJson("unsupported protocol version");
  parsed.type = RequireString(parsed.root.get(), "type", 1, 64);
  if (!VisibleAscii(parsed.type)) InvalidJson("control type is not visible ASCII");
  (void)RequireString(parsed.root.get(), "message_id", 1, 64);
  parsed.device_id = RequireString(parsed.root.get(), "device_id", 1, 64);
  parsed.ids = {RequireUint32(parsed.root.get(), "session_id"), RequireUint32(parsed.root.get(), "turn_id"),
                RequireUint32(parsed.root.get(), "stream_id"), RequireUint32(parsed.root.get(), "epoch")};
  parsed.payload = RequireItem(parsed.root.get(), "payload");
  if (!cJSON_IsObject(parsed.payload)) InvalidJson("payload must be an object");
  return parsed;
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
    client_.set_interrupt_handler([this](Hdl h) { HandlePendingSend(h); });
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
    if (c.message_id.empty() || c.message_id.size() > 64) return false;
    const char* type = nullptr;
    std::string payload;
    switch (c.kind) {
      case ControlKind::kHello:
        if (c.ids.session || c.ids.turn || c.ids.stream || c.ids.epoch)
          return false;
        type = "hello";
        payload = "{\"device_token\":" +
                  Quote(kTeachingDeviceToken) + "}";
        break;
      case ControlKind::kTurnStart:
        if (!ValidIds(c.ids)) return false;
        type = "turn.start";
        payload = "{\"sample_rate_hz\":16000}";
        break;
      case ControlKind::kTurnCommit:
        if (!ValidIds(c.ids)) return false;
        type = "turn.commit";
        payload = "{}";
        break;
      case ControlKind::kTurnCancel:
        if (!ValidIds(c.ids)) return false;
        type = "turn.cancel";
        payload = "{}";
        break;
      case ControlKind::kResponseCancel:
        if (!ValidIds(c.ids)) return false;
        type = "response.cancel";
        payload = "{}";
        break;
    }
    std::string json = "{\"version\":1,\"type\":\"" + std::string(type) +
        "\",\"message_id\":" + Quote(c.message_id) +
        ",\"device_id\":" + Quote(config_.device_id) +
        ",\"session_id\":" + std::to_string(c.ids.session) +
        ",\"turn_id\":" + std::to_string(c.ids.turn) +
        ",\"stream_id\":" + std::to_string(c.ids.stream) +
        ",\"epoch\":" + std::to_string(c.ids.epoch) +
        ",\"payload\":" + payload + "}";
    const bool cancellation = c.kind == ControlKind::kTurnCancel ||
                              c.kind == ControlKind::kResponseCancel;
    return SendText(json, cancellation);
  }

  bool SendText(const std::string& json,
                const bool cancellation = false) {
    if (!connected_ || json.empty() || json.size() > 64U * 1024U) return false;
    // 每轮最多发一个取消控制帧。即使 PCM 已达到背压线，也允许这一个小帧排在
    // 已接受的音频之后，避免把局部拥塞放大成 TLS/provider 会话重建。
    return DispatchSend(json.data(), json.size(),
                        websocketpp::frame::opcode::text,
                        cancellation) == SendStatus::kSent;
  }

  SendOutcome SendPcm(const Pcm64Header& h, const std::uint8_t* pcm,
                      std::size_t bytes) {
    if (!connected_ || !pcm || bytes == 0 ||
        bytes > kMaxUplinkPcmBytes || bytes % 2 ||
        h.flags & ~kKnownFlags || !ValidIds(h.ids) ||
        h.sequence == std::numeric_limits<std::uint32_t>::max() ||
        ((h.sequence == 0) != ((h.flags & 1U) != 0))) {
      return SendOutcome::kRejected;
    }
    // 字段逐个按网络字节序写入，不能直接发送 C++ struct（padding/endianness 不稳定）。
    std::array<std::uint8_t, kHeaderBytes + kMaxUplinkPcmBytes> frame;
    auto* p = frame.data();
    std::memcpy(p, "BPV1", 4);
    p[4] = 1;
    p[5] = 1;
    Put16(h.flags, p + 6);
    Put16(64, p + 8);
    p[10] = 1;
    p[11] = 1;
    Put32(16000U, p + 12);
    Put32(static_cast<std::uint32_t>(bytes), p + 16);
    Put32(h.sequence, p + 20);
    Put64(h.timestamp_us, p + 24);
    Put32(h.ids.epoch, p + 32);
    std::memcpy(p + 36, uuid_.data(), uuid_.size());
    Put32(h.ids.session, p + 52);
    Put32(h.ids.turn, p + 56);
    Put32(h.ids.stream, p + 60);
    std::memcpy(p + 64, pcm, bytes);
    // 缓冲满只是瞬时背压：本帧未发、sequence 不前移，由调用方取消当前 turn。
    const SendStatus status = DispatchSend(
        frame.data(), kHeaderBytes + bytes,
        websocketpp::frame::opcode::binary, false);
    if (status == SendStatus::kFull) return SendOutcome::kBackpressure;
    return status == SendStatus::kSent ? SendOutcome::kOk
                                       : SendOutcome::kRejected;
  }

  void Close() noexcept {
    // closing_ 保证多条失败/信号路径只发一次 close；stop 还会取消尚未完成的 DNS/TCP/TLS，
    // 避免 connected=false 时只停 perpetual work、却在 join 中永远等待 pending connect。
    if (!closing_.exchange(true)) {
      websocketpp::lib::error_code ec;
      const bool was_connected = connected_.exchange(false);
      CancelPendingSend();
      if (was_connected) client_.close(hdl_, websocketpp::close::status::normal, "", ec);
      client_.stop_perpetual();
      client_.stop();
    }
    if (thread_.joinable() && thread_.get_id() != std::this_thread::get_id()) thread_.join();
  }
  bool connected() const noexcept { return connected_; }

 private:
  struct PendingSend final {
    std::array<std::uint8_t, kMaxMessageBytes> bytes{};
    std::size_t size{0U};
    websocketpp::frame::opcode::value opcode{
        websocketpp::frame::opcode::text};
    SendStatus status{SendStatus::kUnavailable};
    bool cancellation{false};
    bool queued{false};
    bool done{false};
  };

  static bool ValidIds(const WireIds& i) { return i.session && i.turn && i.stream && i.epoch; }
  SendStatus DispatchSend(const void* const bytes, const std::size_t size,
                          const websocketpp::frame::opcode::value opcode,
                          const bool cancellation) noexcept {
    std::lock_guard<std::mutex> submit_lock(send_mutex_);
    if (!connected_.load(std::memory_order_acquire) ||
        closing_.load(std::memory_order_acquire) || bytes == nullptr ||
        size == 0U || size > kMaxMessageBytes) {
      return SendStatus::kUnavailable;
    }
    std::unique_lock<std::mutex> lock(pending_mutex_);
    if (pending_.queued) return SendStatus::kUnavailable;
    std::memcpy(pending_.bytes.data(), bytes, size);
    pending_.size = size;
    pending_.opcode = opcode;
    pending_.status = SendStatus::kUnavailable;
    pending_.cancellation = cancellation;
    pending_.queued = true;
    pending_.done = false;
    websocketpp::lib::error_code ec;
    client_.interrupt(hdl_, ec);
    if (ec) {
      pending_.queued = false;
      return SendStatus::kUnavailable;
    }
    if (!pending_changed_.wait_for(lock, kSendDispatchWait,
                                   [this] { return pending_.done; })) {
      // A rejected request must never be sent later: that would advance the
      // wire sequence after the application has already cancelled the turn.
      pending_.queued = false;
      return SendStatus::kUnavailable;
    }
    return pending_.status;
  }

  void HandlePendingSend(const Hdl h) noexcept {
    std::unique_lock<std::mutex> lock(pending_mutex_);
    if (!pending_.queued) return;
    SendStatus status = SendStatus::kUnavailable;
    try {
      websocketpp::lib::error_code ec;
      const auto connection = client_.get_con_from_hdl(h, ec);
      if (!ec && connection && connected_.load(std::memory_order_acquire) &&
          !closing_.load(std::memory_order_acquire)) {
        const std::size_t queued = connection->get_buffered_amount();
        const bool full = queued > kMaxBufferedBytes ||
                          pending_.size > kMaxBufferedBytes - queued;
        if (full && !pending_.cancellation) {
          status = SendStatus::kFull;
        } else {
          ec = connection->send(pending_.bytes.data(), pending_.size,
                                pending_.opcode);
          if (!ec) status = SendStatus::kSent;
        }
      }
    } catch (...) {
    }
    pending_.status = status;
    pending_.queued = false;
    pending_.done = true;
    lock.unlock();
    pending_changed_.notify_one();
  }

  void CancelPendingSend() noexcept {
    std::lock_guard<std::mutex> lock(pending_mutex_);
    if (!pending_.queued) return;
    pending_.status = SendStatus::kUnavailable;
    pending_.queued = false;
    pending_.done = true;
    pending_changed_.notify_one();
  }
  void Fail(std::string why) {
    connected_ = false;
    CancelPendingSend();
    if (error_) try {
      error_(std::move(why));
    } catch (...) {
    }
  }
  void Emit(Inbound event) {
    if (inbound_) try {
      inbound_(std::move(event));
    } catch (...) {
      Fail("inbound callback threw");
    }
  }

  bool DecodePin(const std::string& text) {
    if (!boompi::config::IsValidSpkiSha256(text)) return false;
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

  void OnText(const std::string& json) {
    // 传输层只把一帧解码成有类型的事件。跨帧的 turn/epoch/response
    // 顺序由 application actor 单线程判定，避免这里再隐藏一套会话状态机。
    try {
      const ParsedEnvelope parsed = ParseEnvelope(json);
      const std::string& type = parsed.type;
      const cJSON* body = parsed.payload;
      Inbound e;
      e.ids = parsed.ids;
      if (parsed.device_id != config_.device_id)
        throw std::runtime_error("wrong device identity");
      if (type == "hello.ack") {
        e.kind = InboundKind::kHelloAck;
        if (RequireUint32(body, "input_sample_rate_hz") != 16000 ||
            RequireUint32(body, "output_sample_rate_hz") != 24000 ||
            RequireUint32(body, "input_frame_ms") != 20)
          throw std::runtime_error("unsupported hello audio contract");
      } else if (type == "response.start") {
        e.kind = InboundKind::kResponseStart;
        e.response_id = RequireString(body, "response_id", 1, 128);
      } else if (type == "response.text_delta") {
        e.kind = InboundKind::kTextDelta;
        e.response_id = RequireString(body, "response_id", 1, 128);
        e.text = RequireString(body, "text", 1, 4096);
      } else if (type == "response.audio_start") {
        e.kind = InboundKind::kAudioStart;
        e.response_id = RequireString(body, "response_id", 1, 128);
        if (RequireUint32(body, "sample_rate_hz") != 24000)
          throw std::runtime_error("unsupported response audio rate");
      } else if (type == "response.cancelled") {
        e.kind = InboundKind::kCancelled;
        e.text = RequireString(body, "reason", 1, 64);
      } else if (type == "response.done") {
        e.kind = InboundKind::kDone;
        e.response_id = RequireString(body, "response_id", 1, 128);
      } else if (type == "error") {
        e.kind = InboundKind::kError;
        e.error_code = RequireString(body, "code", 1, 64);
        e.text = RequireString(body, "message", 1, 512);
      } else {
        throw std::runtime_error("unsupported control type");
      }
      Emit(std::move(e));
    } catch (const std::exception& e) {
      ProtocolError(e.what());
    }
  }

  void OnBinary(const std::string& bytes) {
    // 只校验单帧布局、长度、设备 UUID 与声学格式。序号连续性和当前
    // response 身份由 application actor 校验，因为它才是会话的唯一 owner。
    const auto* p = reinterpret_cast<const std::uint8_t*>(bytes.data());
    if (bytes.size() < kHeaderBytes ||
        std::memcmp(p, "BPV1", 4) != 0 || p[4] != 1 || p[5] != 2 ||
        Get16(p + 8) != 64 || p[10] != 1 || p[11] != 1) {
      return ProtocolError("invalid PCM header");
    }
    const std::size_t payload = Get32(p + 16);
    const std::uint16_t flags = Get16(p + 6);
    Inbound e;
    e.kind = InboundKind::kPcm;
    e.pcm.flags = flags;
    e.pcm.sequence = Get32(p + 20);
    e.pcm.timestamp_us = Get64(p + 24);
    e.pcm.ids = {Get32(p + 52), Get32(p + 56), Get32(p + 60),
                 Get32(p + 32)};
    e.ids = e.pcm.ids;
    if (payload == 0 || payload > kMaxDownlinkPcmBytes || payload % 2 ||
        bytes.size() != kHeaderBytes + payload || flags & ~kKnownFlags ||
        Get32(p + 12) != 24000 ||
        std::memcmp(p + 36, uuid_.data(), uuid_.size()) != 0 ||
        e.pcm.sequence == std::numeric_limits<std::uint32_t>::max() ||
        ((e.pcm.sequence == 0U) != ((flags & 1U) != 0U))) {
      return ProtocolError("invalid PCM framing");
    }
    std::copy_n(p + kHeaderBytes, payload, e.audio.begin());
    e.audio_size = payload;
    Emit(std::move(e));
  }

  void OnMessage(const Client::message_ptr& message) {
    if (message->get_payload().size() > kMaxMessageBytes) {
      return ProtocolError("message too large");
    }
    if (message->get_opcode() == websocketpp::frame::opcode::text) {
      OnText(message->get_payload());
    } else if (message->get_opcode() == websocketpp::frame::opcode::binary) {
      OnBinary(message->get_payload());
    } else {
      ProtocolError("unsupported WebSocket opcode");
    }
  }

  void ProtocolError(const std::string& why) {
    Fail(why);
    websocketpp::lib::error_code ec;
    client_.close(hdl_, websocketpp::close::status::policy_violation,
                  "protocol error", ec);
  }

  // ASIO 线程拥有 client_/hdl_ 回调；send_mutex_ 只串行化 websocketpp 发送。
  // 这里刻意没有 turn/response 字段，会话数据只属于 application actor。
  Client client_;
  Hdl hdl_;
  TransportConfig config_;
  InboundHandler inbound_;
  ErrorHandler error_;
  std::thread thread_;
  std::mutex send_mutex_;
  std::mutex pending_mutex_;
  std::condition_variable pending_changed_;
  PendingSend pending_;
  std::atomic<bool> connected_{false};
  std::atomic<bool> closing_{false};
  std::array<std::uint8_t, 16> uuid_{};
  std::array<unsigned char, 32> pin_{};
};

VoiceTransport::VoiceTransport() : impl_(std::make_unique<Impl>()) {}
VoiceTransport::~VoiceTransport() = default;
bool VoiceTransport::Connect(TransportConfig config, InboundHandler inbound,
                             ErrorHandler error) {
  return impl_->Connect(std::move(config), std::move(inbound),
                        std::move(error));
}
bool VoiceTransport::SendControl(const Control& control) {
  return impl_->Send(control);
}
SendOutcome VoiceTransport::SendPcm64(const Pcm64Header& header,
                                      const std::uint8_t* payload,
                                      const std::size_t bytes) {
  return impl_->SendPcm(header, payload, bytes);
}
void VoiceTransport::Close() noexcept {
  impl_->Close();
}
bool VoiceTransport::connected() const noexcept {
  return impl_->connected();
}

}  // namespace boompi::network
