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
using boompi::network::LinkConfig;
using boompi::network::LinkEvent;
using boompi::network::LinkEventKind;
using boompi::network::SendResult;
using boompi::network::VoiceLink;

constexpr char kDeviceId[] = "00112233-4455-4677-8899-aabbccddeeff";

void Check(const bool condition, const char* const message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

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

struct TestIdentity final {
  std::string certificate_pem;
  std::string private_key_pem;
  std::string spki_pin;
};

std::string BioText(BIO* const bio) {
  BUF_MEM* memory = nullptr;
  BIO_get_mem_ptr(bio, &memory);
  if (memory == nullptr || memory->data == nullptr || memory->length == 0U) {
    throw std::runtime_error("OpenSSL produced empty PEM");
  }
  return {memory->data, memory->length};
}

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

struct CapturedMessage final {
  websocketpp::frame::opcode::value opcode{};
  std::string payload;
};

class LoopbackServer final {
  using Server = websocketpp::server<websocketpp::config::asio_tls>;
  using Hdl = websocketpp::connection_hdl;

 public:
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
        server_.send(handle, "{\"type\":\"ready\"}", websocketpp::frame::opcode::text, ec);
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

  ~LoopbackServer() {
    server_.stop();
    if (thread_.joinable()) {
      thread_.join();
    }
  }

  LinkConfig Config() const {
    return {kDeviceId, "127.0.0.1", port_, identity_.spki_pin};
  }

  void Send(std::string text, bool binary = false) {
    server_.get_io_service().post([this, text = std::move(text), binary] {
      websocketpp::lib::error_code ec;
      server_.send(
          handle_, text,
          binary ? websocketpp::frame::opcode::binary : websocketpp::frame::opcode::text, ec);
    });
  }

  void Drop() {
    server_.get_io_service().post([this] {
      websocketpp::lib::error_code ec;
      server_.close(handle_, websocketpp::close::status::going_away, "", ec);
    });
  }

  void PauseReads(bool pause) {
    server_.get_io_service().post([this, pause] {
      websocketpp::lib::error_code ec;
      if (pause) {
        // A websocketpp handle is weak. Pausing removes the pending read that
        // normally owns the connection; retain it while the reader is paused.
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

LinkEvent WaitEvent(VoiceLink& link, unsigned timeout_ms = 3000) {
  const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
  LinkEvent event;
  while (Clock::now() < deadline) {
    if (link.Poll(&event)) {
      return event;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  throw std::runtime_error("link event timeout");
}

void Open(VoiceLink& link, const LoopbackServer& server) {
  const auto started = Clock::now();
  Check(link.Open(server.Config()), "Open rejected");
  Check(Clock::now() - started < std::chrono::milliseconds(200), "Open blocked on networking");
  Check(WaitEvent(link).kind == LinkEventKind::Online, "ready did not become Online");
}

std::uint32_t Read32(const std::string& bytes, std::size_t offset) {
  const auto* p = reinterpret_cast<const unsigned char*>(bytes.data() + offset);
  return (static_cast<std::uint32_t>(p[0]) << 24U) | (static_cast<std::uint32_t>(p[1]) << 16U) |
         (static_cast<std::uint32_t>(p[2]) << 8U) | p[3];
}

// Independent server-side wire fixture: test never calls the production encoder.
std::string Audio(std::uint32_t generation, std::uint32_t sequence, unsigned flags,
                  std::size_t samples = 480) {
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

void Upload(VoiceLink& link, std::uint32_t generation, bool supersede = false) {
  std::array<std::int16_t, 320> pcm{};
  pcm[0] = 0x1234;
  pcm[1] = -2;
  Check(link.SendAudio(generation, pcm.data(), true, true, supersede) == SendResult::Ok,
        "complete input was rejected");
}

void BasicWireAndClose() {
  LoopbackServer server;
  VoiceLink link;
  Open(link, server);
  Upload(link, 1);
  const auto messages = server.WaitMessages(2);
  Check(messages[0].payload == "{\"type\":\"hello\",\"device_id\":\"" + std::string(kDeviceId) +
                                   "\",\"token\":\"boompi-teaching-shared-token-v1-2026\"}",
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
            event.audio_size == 960,
        "first audio lost");
  event = WaitEvent(link);
  Check(
      event.kind == LinkEventKind::Audio && !event.start && event.end && event.audio_size == 14,
      "tail audio lost");
  Check(WaitEvent(link).kind == LinkEventKind::Done, "done missing");
  const auto close_started = Clock::now();
  link.Close();
  Check(Clock::now() - close_started < std::chrono::milliseconds(500), "Close was not bounded");
  Check(!link.Poll(&event), "Close kept stale events");
}

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

void RejectBrokenWire() {
  const std::vector<std::vector<std::pair<std::string, bool>>> cases{
      {{Audio(1, 0, 1), true}, {Audio(1, 2, 2), true}},
      {{Audio(1, 0, 1), true}, {"{\"type\":\"done\",\"generation\":1}", false}},
      {{Audio(1, 0, 3), true}, {Audio(1, 1, 2), true}},
      {{"{\"type\":\"text\",\"generation\":2,\"text\":\"future\"}", false}},
      {{"{\"type\":\"ready\"}", false}},
      {{"{\"type\":\"text\",\"generation\":1,\"generation\":1,\"text\":\"duplicate\"}", false}},
      {{Audio(1, 0, 1, 1), true}},
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

void TlsFailureAndReconnect() {
  LoopbackServer server;
  VoiceLink wrong_pin;
  auto config = server.Config();
  config.spki[0] = config.spki[0] == 'A' ? 'B' : 'A';
  Check(wrong_pin.Open(config), "TLS failure did not start");
  auto event = WaitEvent(wrong_pin);
  Check(event.kind == LinkEventKind::Offline && event.code == "tls_connect",
        "pin mismatch not reported");
  wrong_pin.Close();

  VoiceLink link;
  Open(link, server);
  server.Drop();
  Check(WaitEvent(link).kind == LinkEventKind::Offline, "disconnect not reported");
  Check(WaitEvent(link).kind == LinkEventKind::Online, "automatic reconnect failed");
  Upload(link, 5);
}

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
  // A new generation filters both queued old PCM and late old replies.
  server.Send("{\"type\":\"done\",\"generation\":3}");
  const auto done = WaitEvent(link);
  Check(done.kind == LinkEventKind::Done && done.generation == 3, "full-queue fence failed");

  // Input START alone is queued; enough output to overflow the application's receive queue.
  Upload(link, 4);
  for (unsigned i = 0; i < 65; ++i) {
    server.Send("{\"type\":\"text\",\"generation\":4,\"text\":\"x\"}");
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  const auto offline = WaitEvent(link);
  Check(offline.kind == LinkEventKind::Offline && offline.code == "inbound_overflow",
        "receive overflow was hidden");
}

void HandshakeTimeout() {
  LoopbackServer server(false);
  VoiceLink link;
  Check(link.Open(server.Config()), "no-ready Open rejected");
  const auto event = WaitEvent(link, 6500);
  Check(event.kind == LinkEventKind::Offline && event.code == "hello_timeout",
        "missing ready was not bounded");
}

void PendingRetirementKeepsMeaning() {
  LoopbackServer server;
  VoiceLink link;
  Open(link, server);
  // Each pair is submitted immediately: no ACK or socket wait may separate it.
  // STOP retract must remain before NORMAL even when both are still queued.
  Upload(link, 1);
  server.WaitMessages(2);
  Check(link.Stop(2, true), "pending retract STOP failed");
  Upload(link, 3);
  auto messages = server.WaitMessages(4);
  Check(messages[2].payload == "{\"type\":\"stop\",\"generation\":2,\"retract\":true}" &&
            Read32(messages[3].payload, 8) == 3,
        "normal START erased pending history retraction");

  // START|SUPERSEDE has the same history side effect as a retracting STOP.
  // A quick stop must not discard it while purging old PCM.
  Upload(link, 4, true);
  Check(link.Stop(5, false), "stop after pending supersede failed");
  Upload(link, 6);
  messages = server.WaitMessages(7);
  Check(messages[4].payload[5] == 7 && Read32(messages[4].payload, 8) == 4 &&
            messages[5].payload == "{\"type\":\"stop\",\"generation\":5,\"retract\":false}" &&
            Read32(messages[6].payload, 8) == 6,
        "new fence erased supersede history retraction");

  // Pause the real TLS reader, so the small receive window blocks socket writes.
  // Preserve STOP and supersede in order while ordinary queued PCM is retired.
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
      // A TCP zero-window probe can exceed the 800ms retirement deadline.
      // The protocol then requires explicit failure, never silent marker loss.
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

void ExternalServerSmoke(const char* host, const char* port, const char* pin) {
  const auto parsed_port = std::stoul(port);
  Check(parsed_port > 0 && parsed_port <= 65535, "invalid smoke port");
  LinkConfig config{kDeviceId, host, static_cast<std::uint16_t>(parsed_port), pin};
  auto bad_config = config;
  bad_config.spki[0] = bad_config.spki[0] == 'A' ? 'B' : 'A';
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

int main(int argc, char** argv) {
  try {
    if (argc == 4) {
      ExternalServerSmoke(argv[1], argv[2], argv[3]);
      return EXIT_SUCCESS;
    }
    BasicWireAndClose();
    GenerationFence();
    RejectBrokenWire();
    TlsFailureAndReconnect();
    BoundedQueuesAndSupersede();
    PendingRetirementKeepsMeaning();
    HandshakeTimeout();
  } catch (const std::exception& error) {
    std::cerr << "voice link loopback: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
  std::cout << "voice link loopback: passed\n";
  return EXIT_SUCCESS;
}
