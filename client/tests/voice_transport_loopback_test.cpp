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

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#include <websocketpp/config/asio.hpp>
#include <websocketpp/server.hpp>

#include "boompi/network/voice_transport.h"

namespace {

using Clock = std::chrono::steady_clock;
using boompi::network::Control;
using boompi::network::ControlKind;
using boompi::network::Inbound;
using boompi::network::InboundKind;
using boompi::network::Pcm64Header;
using boompi::network::SendOutcome;
using boompi::network::TransportConfig;
using boompi::network::VoiceTransport;
using boompi::network::WireIds;

constexpr char kDeviceId[] = "00112233-4455-4677-8899-aabbccddeeff";

void Check(const bool condition, const char* const message) {
  if (!condition) throw std::runtime_error(message);
}

struct BioDelete final {
  void operator()(BIO* value) const noexcept { BIO_free(value); }
};
struct KeyContextDelete final {
  void operator()(EVP_PKEY_CTX* value) const noexcept { EVP_PKEY_CTX_free(value); }
};
struct KeyDelete final {
  void operator()(EVP_PKEY* value) const noexcept { EVP_PKEY_free(value); }
};
struct CertificateDelete final {
  void operator()(X509* value) const noexcept { X509_free(value); }
};
struct ExtensionDelete final {
  void operator()(X509_EXTENSION* value) const noexcept { X509_EXTENSION_free(value); }
};

struct TestIdentity final {
  std::string certificate_pem;
  std::string private_key_pem;
  std::string spki_pin;
};

std::string BioText(BIO* const bio) {
  BUF_MEM* memory = nullptr;
  BIO_get_mem_ptr(bio, &memory);
  if (memory == nullptr || memory->data == nullptr || memory->length == 0U)
    throw std::runtime_error("OpenSSL produced empty PEM");
  return {memory->data, memory->length};
}

TestIdentity MakeTestIdentity() {
  std::unique_ptr<EVP_PKEY_CTX, KeyContextDelete> key_context(
      EVP_PKEY_CTX_new_id(EVP_PKEY_EC, nullptr));
  Check(key_context != nullptr && EVP_PKEY_keygen_init(key_context.get()) == 1 &&
            EVP_PKEY_CTX_set_ec_paramgen_curve_nid(
                key_context.get(), NID_X9_62_prime256v1) == 1,
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
  Check(subject != nullptr &&
            X509_NAME_add_entry_by_txt(
                subject, "CN", MBSTRING_ASC,
                reinterpret_cast<const unsigned char*>("boompi loopback test"),
                -1, -1, 0) == 1 &&
            X509_set_issuer_name(certificate.get(), subject) == 1,
        "could not name test TLS certificate");

  X509V3_CTX extension_context{};
  X509V3_set_ctx(&extension_context, certificate.get(), certificate.get(),
                 nullptr, nullptr, 0);
  const auto add_extension = [&](const int nid, const char* const value) {
    std::unique_ptr<X509_EXTENSION, ExtensionDelete> extension(
        X509V3_EXT_conf_nid(nullptr, &extension_context, nid,
                           const_cast<char*>(value)));
    Check(extension != nullptr &&
              X509_add_ext(certificate.get(), extension.get(), -1) == 1,
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
            PEM_write_bio_PrivateKey(key_bio.get(), key.get(), nullptr, nullptr,
                                     0, nullptr, nullptr) == 1,
        "could not encode test TLS identity");

  X509_PUBKEY* const public_key = X509_get_X509_PUBKEY(certificate.get());
  const int der_size = i2d_X509_PUBKEY(public_key, nullptr);
  Check(public_key != nullptr && der_size > 0, "could not size test SPKI");
  std::vector<unsigned char> der(static_cast<std::size_t>(der_size));
  unsigned char* cursor = der.data();
  Check(i2d_X509_PUBKEY(public_key, &cursor) == der_size,
        "could not encode test SPKI");
  std::array<unsigned char, 32> digest{};
  unsigned int digest_size = 0U;
  Check(EVP_Digest(der.data(), der.size(), digest.data(), &digest_size,
                   EVP_sha256(), nullptr) == 1 &&
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
  using Tls = websocketpp::lib::shared_ptr<
      websocketpp::lib::asio::ssl::context>;

 public:
  LoopbackServer(TestIdentity identity, const bool stall_reads)
      : identity_(std::move(identity)), stall_reads_(stall_reads) {
    server_.clear_access_channels(websocketpp::log::alevel::all);
    server_.clear_error_channels(websocketpp::log::elevel::all);
    server_.init_asio();
    server_.set_reuse_addr(true);
    server_.set_socket_init_handler(
        [](Hdl, websocketpp::lib::asio::ssl::stream<
                    websocketpp::lib::asio::ip::tcp::socket>& socket) {
          websocketpp::lib::asio::error_code ec;
          socket.next_layer().set_option(
              websocketpp::lib::asio::socket_base::receive_buffer_size(1024),
              ec);
        });
    server_.set_tls_init_handler([this](Hdl) {
      auto context = websocketpp::lib::make_shared<
          websocketpp::lib::asio::ssl::context>(
          websocketpp::lib::asio::ssl::context::tls_server);
      SSL_CTX_set_min_proto_version(context->native_handle(), TLS1_2_VERSION);
      context->use_certificate_chain(websocketpp::lib::asio::buffer(
          identity_.certificate_pem));
      context->use_private_key(websocketpp::lib::asio::buffer(
                                   identity_.private_key_pem),
                               websocketpp::lib::asio::ssl::context::pem);
      return context;
    });
    server_.set_open_handler([this](Hdl handle) {
      static_cast<void>(handle);
      {
        std::lock_guard<std::mutex> lock(mutex_);
        open_ = true;
      }
      changed_.notify_all();
      // Keep the server ASIO thread out of reads long enough for the client
      // socket and WebSocket queue to reach the fixed 28 KiB bound.
      if (stall_reads_) std::this_thread::sleep_for(std::chrono::seconds(6));
    });
    server_.set_message_handler(
        [this](Hdl handle, const Server::message_ptr& message) {
          const bool hello = message->get_opcode() ==
                                     websocketpp::frame::opcode::text &&
              message->get_payload().find("\"type\":\"hello\"") !=
                  std::string::npos;
          {
            std::lock_guard<std::mutex> lock(mutex_);
            messages_.push_back(
                {message->get_opcode(), message->get_payload()});
          }
          changed_.notify_all();
          if (hello) {
            const std::string ack =
                "{\"version\":1,\"type\":\"hello.ack\","
                "\"message_id\":\"server-1\",\"device_id\":\"" +
                std::string(kDeviceId) +
                "\",\"session_id\":7,\"turn_id\":0,\"stream_id\":0,"
                "\"epoch\":1,\"payload\":{\"input_sample_rate_hz\":16000,"
                "\"output_sample_rate_hz\":24000,\"input_frame_ms\":20}}";
            websocketpp::lib::error_code ec;
            server_.send(handle, ack, websocketpp::frame::opcode::text, ec);
          }
        });

    websocketpp::lib::error_code ec;
    server_.listen(websocketpp::lib::asio::ip::tcp::endpoint(
                       websocketpp::lib::asio::ip::address_v4::loopback(), 0),
                   ec);
    Check(!ec, "loopback WSS listen failed");
    websocketpp::lib::asio::error_code endpoint_error;
    port_ = server_.get_local_endpoint(endpoint_error).port();
    Check(!endpoint_error && port_ != 0U,
          "loopback WSS port lookup failed");
    server_.start_accept(ec);
    Check(!ec, "loopback WSS accept failed");
    thread_ = std::thread([this] { server_.run(); });
  }

  ~LoopbackServer() { Stop(); }

  LoopbackServer(const LoopbackServer&) = delete;
  LoopbackServer& operator=(const LoopbackServer&) = delete;

  std::uint16_t port() const noexcept { return port_; }
  const std::string& pin() const noexcept { return identity_.spki_pin; }

  bool WaitOpen() {
    std::unique_lock<std::mutex> lock(mutex_);
    return changed_.wait_for(lock, std::chrono::seconds(2),
                             [this] { return open_; });
  }

  std::vector<CapturedMessage> WaitMessages(const std::size_t count) {
    std::unique_lock<std::mutex> lock(mutex_);
    Check(changed_.wait_for(lock, std::chrono::seconds(2),
                            [this, count] { return messages_.size() >= count; }),
          "loopback WSS did not receive expected messages");
    return messages_;
  }

  void Stop() noexcept {
    if (!thread_.joinable()) return;
    websocketpp::lib::error_code ec;
    server_.stop_listening(ec);
    server_.stop();
    thread_.join();
  }

 private:
  TestIdentity identity_;
  bool stall_reads_{false};
  Server server_;
  std::thread thread_;
  std::mutex mutex_;
  std::condition_variable changed_;
  std::vector<CapturedMessage> messages_;
  std::uint16_t port_{0U};
  bool open_{false};
};

bool WaitConnected(const VoiceTransport& transport) {
  const auto deadline = Clock::now() + std::chrono::seconds(2);
  while (!transport.connected() && Clock::now() < deadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  return transport.connected();
}

TransportConfig ConfigFor(const LoopbackServer& server) {
  TransportConfig config{};
  config.host = "127.0.0.1";
  config.port = server.port();
  config.device_id = kDeviceId;
  config.spki_sha256_base64 = server.pin();
  return config;
}

std::uint32_t Read32(const std::string& bytes, const std::size_t offset) {
  const auto* p = reinterpret_cast<const unsigned char*>(bytes.data() + offset);
  return (static_cast<std::uint32_t>(p[0]) << 24U) |
         (static_cast<std::uint32_t>(p[1]) << 16U) |
         (static_cast<std::uint32_t>(p[2]) << 8U) |
         static_cast<std::uint32_t>(p[3]);
}

void BasicWireAndClose() {
  LoopbackServer server(MakeTestIdentity(), false);
  std::mutex inbound_mutex;
  std::condition_variable inbound_changed;
  bool hello_ack = false;
  std::string transport_error;
  VoiceTransport transport;
  Check(transport.Connect(
            ConfigFor(server),
            [&](Inbound event) {
              if (event.kind != InboundKind::kHelloAck) return;
              {
                std::lock_guard<std::mutex> lock(inbound_mutex);
                hello_ack = true;
              }
              inbound_changed.notify_one();
            },
            [&](std::string error) { transport_error = std::move(error); }),
        "production transport Connect rejected loopback config");
  Check(WaitConnected(transport) && server.WaitOpen(),
        "production transport did not open loopback WSS");

  Control hello{};
  hello.kind = ControlKind::kHello;
  hello.message_id = "client-1";
  Check(transport.SendControl(hello), "hello send failed");
  {
    std::unique_lock<std::mutex> lock(inbound_mutex);
    Check(inbound_changed.wait_for(lock, std::chrono::seconds(2),
                                   [&] { return hello_ack; }),
          "production parser did not emit hello.ack");
  }

  const WireIds ids{7U, 1U, 1U, 1U};
  Control start{};
  start.kind = ControlKind::kTurnStart;
  start.message_id = "client-2";
  start.ids = ids;
  Check(transport.SendControl(start), "turn.start send failed");
  std::array<std::uint8_t, 640> pcm{};
  pcm[0] = 7U;
  pcm[1] = 8U;
  Pcm64Header header{};
  header.flags = 3U;
  header.sequence = 0U;
  header.timestamp_us = 123456U;
  header.ids = ids;
  Check(transport.SendPcm64(header, pcm.data(), pcm.size()) == SendOutcome::kOk,
        "PCM send failed");
  Control commit{};
  commit.kind = ControlKind::kTurnCommit;
  commit.message_id = "client-3";
  commit.ids = ids;
  Check(transport.SendControl(commit), "turn.commit send failed");

  const auto messages = server.WaitMessages(4U);
  Check(messages[0].opcode == websocketpp::frame::opcode::text &&
            messages[0].payload.find("\"type\":\"hello\"") !=
                std::string::npos,
        "hello wire frame mismatch");
  Check(messages[1].opcode == websocketpp::frame::opcode::text &&
            messages[1].payload.find("\"type\":\"turn.start\"") !=
                std::string::npos,
        "turn.start wire frame mismatch");
  Check(messages[2].opcode == websocketpp::frame::opcode::binary &&
            messages[2].payload.size() == 704U &&
            std::memcmp(messages[2].payload.data(), "BPV1", 4U) == 0 &&
            Read32(messages[2].payload, 16U) == pcm.size() &&
            Read32(messages[2].payload, 20U) == 0U &&
            static_cast<unsigned char>(messages[2].payload[64]) == 7U &&
            static_cast<unsigned char>(messages[2].payload[65]) == 8U,
        "PCM64 wire frame mismatch");
  Check(messages[3].opcode == websocketpp::frame::opcode::text &&
            messages[3].payload.find("\"type\":\"turn.commit\"") !=
                std::string::npos,
        "turn.commit wire frame mismatch");

  const auto close_started = Clock::now();
  transport.Close();
  Check(Clock::now() - close_started < std::chrono::milliseconds(500) &&
            !transport.connected(),
        "production transport Close was not bounded");
  Check(transport_error.empty(), "happy-path transport reported an error");
}

void BackpressureKeepsControlAvailable() {
  LoopbackServer server(MakeTestIdentity(), true);
  VoiceTransport transport;
  Check(transport.Connect(ConfigFor(server), [](Inbound) {}, [](std::string) {}),
        "backpressure transport Connect failed");
  Check(WaitConnected(transport) && server.WaitOpen(),
        "backpressure WSS did not open");

  std::array<std::uint8_t, 640> pcm{};
  Pcm64Header header{};
  header.ids = {7U, 1U, 1U, 1U};
  bool full = false;
  const auto deadline = Clock::now() + std::chrono::seconds(5);
  for (std::uint32_t sequence = 0U;
       sequence != 20000U && Clock::now() < deadline; ++sequence) {
    header.sequence = sequence;
    header.flags = sequence == 0U ? 1U : 0U;
    const SendOutcome outcome =
        transport.SendPcm64(header, pcm.data(), pcm.size());
    if (outcome == SendOutcome::kBackpressure) {
      full = true;
      break;
    }
    if (outcome != SendOutcome::kOk) {
      throw std::runtime_error(
          "stalled WSS returned rejection instead of backpressure at sequence " +
          std::to_string(sequence) + "; connected=" +
          (transport.connected() ? "yes" : "no"));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  Check(full, "stalled WSS did not reach the fixed transport bound");

  Control commit{};
  commit.kind = ControlKind::kTurnCommit;
  commit.message_id = "client-commit";
  commit.ids = header.ids;
  Check(transport.SendControl(commit),
        "bounded PCM queue rejected the turn.commit control frame");

  Control cancel{};
  cancel.kind = ControlKind::kTurnCancel;
  cancel.message_id = "client-cancel";
  cancel.ids = header.ids;
  Check(transport.SendControl(cancel),
        "bounded queue did not preserve the cancellation slot");
  const auto close_started = Clock::now();
  transport.Close();
  Check(Clock::now() - close_started < std::chrono::milliseconds(500),
        "stalled transport Close was not bounded");
}

}  // namespace

int main() {
  try {
    BasicWireAndClose();
    BackpressureKeepsControlAvailable();
  } catch (const std::exception& error) {
    std::cerr << "voice transport loopback: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
  std::cout << "voice transport loopback: passed\n";
  return EXIT_SUCCESS;
}
