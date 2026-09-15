/**
 * @file voice_link.cpp
 * @brief 持久 WSS 连接：线程内完成建链、握手、收发、心跳和重连。
 *
 * 应用线程只投递语音和 STOP，或取走协议事件。共享队列与 generation 用同一把锁
 * 保护，WebSocket++ 和 TLS 对象只由网络线程访问。
 *
 * 启动链：Open → NetworkTask → FindServer → Connect → TLS 公钥校验 → SendHello → ready。
 * 上行链：SendAudio/Stop → outbound_ → SendNextFrame → WSS；下行链：OnMessage →
 * DecodeText/DecodeAudio → OnEvent/AcceptReplyLocked → events_ → 应用 PollEvent。
 * FailLocked 将故障变成 Offline，NetworkTask 丢弃本轮状态后退避重连；不续传断线前的问答。
 */
#include "boompi/network/voice_link.h"

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

namespace boompi::network {

/// @brief 网络工作线程与 application actor 之间的有界数据交接及连接生命周期。
class VoiceLink::Impl final {
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
   * @brief 已拥有线协议字节的待发帧；普通 PCM 与 STOP 共用同一个发送顺序。
   *
   * retirement 标记携带“退休旧轮次”副作用的 STOP 或 START|SUPERSEDE，
   * 后续切代不能像普通 PCM 一样将其删除；queued_at 用于限制滞留时间。
   */
  struct Outbound final {
    std::array<std::uint8_t, detail::kFrameBytes> bytes{};
    std::size_t size{0};
    bool text{false};
    bool retirement{false};
    Clock::time_point queued_at{Clock::now()};
  };

 public:
  ~Impl() {
    Close();
  }

  /**
   * @brief 应用线程校验配置并启动 NetworkTask；真正联网和失败通知均在工作线程完成。
   *
   * UUID 可安全直接放入 hello 的 JSON 字符串；显式 pin 必须与显式 IPv4 地址配套，
   * 避免出现“自动发现地址却使用含义不明的手工 pin”的配置组合。
   */
  bool Open(const config::VoiceClientConfig& settings) {
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
      thread_ = std::thread(&Impl::NetworkTask, this);
    } catch (...) {
      return false;
    }
    return true;
  }

  /// @brief 应用线程短暂持锁取走一条事件；空队列不等待，事件字符串通过 move 移交。
  bool PollEvent(LinkEvent* event) {
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

  /**
   * @brief 应用 actor 将一帧 PCM 原子地校验、切代、编码和入队，不执行 socket I/O。
   *
   * START 从 sequence 0 开始；续帧只接受仍在上传的当前代，END 后不允许再补帧。
   * 只有入队成功才递增 sequence，因此 Backpressure 不会暗中跳过一帧。
   */
  SendResult SendAudio(std::uint32_t generation, const std::int16_t* pcm, bool start, bool end,
                       bool supersede) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (connection_state_ != ConnectionState::Online || stop_.load()) {
      return SendResult::Disconnected;
    }

    bool valid_turn = false;
    if (start) {
      valid_turn = generation > generation_;
    } else {
      valid_turn = generation == generation_ && turn_phase_ == TurnPhase::Uploading &&
                   uplink_sequence_ != UINT32_MAX;
    }
    if (pcm == nullptr || generation == 0 || !valid_turn || (supersede && !start)) {
      FailLocked("invalid_uplink");
      return SendResult::Disconnected;
    }

    // 普通音频满时把背压交给应用处理；打断先退休旧 PCM，再检查保留下来的控制帧。
    if (!supersede && outbound_.size() + kTransportFrames >= kQueueFrames) {
      return SendResult::Backpressure;
    }
    if (start) {
      // 切代必须先于新帧入队，网络接收回调才不会把旧回答放到新代事件之后。
      AdvanceLocked(generation, TurnPhase::Uploading);
    }
    if (outbound_.size() + kTransportFrames >= kQueueFrames) {
      FailLocked("retirement_overflow");
      return SendResult::Disconnected;
    }

    Outbound frame;
    frame.retirement = supersede;
    frame.bytes = detail::EncodeAudio(generation, uplink_sequence_, pcm, start, end, supersede);
    frame.size = frame.bytes.size();
    outbound_.push_back(std::move(frame));
    ++uplink_sequence_;
    if (end) {
      turn_phase_ = TurnPhase::WaitingReply;
    }
    changed_.notify_one();
    return SendResult::Ok;
  }

  /**
   * @brief 不开启新输入的切代路径；应用停止扬声器后调用它终止远端旧轮次。
   *
   * 先清理旧普通 PCM，为短 STOP 留出空间；若保留的退休标记本身已经占满容量，
   * 必须断开连接，不能报告停止成功后悄悄丢掉某次撤回意图。
   */
  bool Stop(std::uint32_t generation, bool retract) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (connection_state_ != ConnectionState::Online || stop_.load()) {
      return false;
    }
    if (generation == 0 || generation <= generation_) {
      FailLocked("invalid_stop");
      return false;
    }
    AdvanceLocked(generation, TurnPhase::Retired);
    if (outbound_.size() >= kQueueFrames) {
      FailLocked("retirement_overflow");
      return false;
    }

    std::string text =
        "{\"type\":\"stop\",\"generation\":" + std::to_string(generation) + ",\"retract\":";
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

  /**
   * @brief 先发退出信号并等待网络线程释放 socket，随后才清理跨线程队列。
   *
   * join 在锁外执行：工作线程退出仍需取得 mutex_，先持锁再 join 会死锁。
   * stop_ 同时供 DHCP 等网络准备步骤检查，避免关闭时继续开始下一次重连。
   */
  void Close() noexcept {
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

 private:
  /**
   * @brief 在锁内切代，让新 START 与旧事件过滤使用同一个边界。
   *
   * STOP 或 SUPERSEDE 的 START 可能尚未上网，但它们携带撤回历史的意图。
   * 连续切代时保留这些标记的顺序，只清掉旧普通 PCM；否则紧接着的普通问题
   * 可能绕过前一次撤回，让服务器把用户没听完的回答保留在历史中。
   */
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

  /**
   * @brief 持有 mutex_ 时统一结束本次连接，并用一条 Offline 替换未消费事件。
   *
   * 故障后的残留字幕和 PCM 都不再可信；清空可避免应用先播放积压回答再收到错误。
   * Failed 保证同一次连接只报告首个故障阶段，真正 socket 清理由网络线程完成。
   */
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

  /// @brief 尚未持锁的网络回调使用此入口；已持锁路径必须调用 FailLocked 避免递归锁。
  void Fail(const char* code) {
    std::lock_guard<std::mutex> lock(mutex_);
    FailLocked(code);
  }

  /**
   * @brief 网络线程持锁检查跨帧顺序；返回 true 才能把回答交给应用。
   *
   * 当前代普通回答要求上行 END 已入队；Error 可在上传中出现，以便立即结束失败输入。
   * 文本可与音频交错，音频 sequence 必须连续且 END 后不可再有音频；done 必须等音频
   * END，纯文本或空回答可直接 done。旧代合法帧直接忽略，未来代或乱序使连接失败。
   */
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
      return false;  // STOP 的 generation 只用于退休，不能产生回答。
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
      audio_ended_ = event.end;
    } else if (event.kind == LinkEventKind::Done) {
      if (downlink_sequence_ != 0 && !audio_ended_) {
        FailLocked("done_before_audio_end");
        return false;
      }
      turn_phase_ = TurnPhase::Complete;
    } else if (event.kind == LinkEventKind::Error) {
      turn_phase_ = TurnPhase::Complete;
    }
    return true;
  }

  /**
   * @brief 将已经通过单帧解码的消息归入连接状态或回答校验，再放入有界事件队列。
   *
   * ready 只能出现一次，收到它才进入 Online。接收队列满时不选择性丢音频，
   * 而是报告 inbound_overflow，让应用停止播放并等待重连。
   */
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

  /**
   * @brief 用保存的 SPKI 摘要验证课堂自签证书，并检查证书的服务器用途。
   *
   * 设备以公钥 pin 为信任锚，不依赖公网 CA。失败必须终止握手；
   * OpenSSL 通过 C ABI 调用这里，任何 C++ 异常都必须在回调内部收住。
   */
  static int VerifyPin(X509_STORE_CTX* store, void* argument) {
    try {
      auto* self = static_cast<Impl*>(argument);
      X509* certificate = X509_STORE_CTX_get0_cert(store);
      if (self == nullptr || certificate == nullptr ||
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
          CRYPTO_memcmp(digest.data(), self->pin_.data(), digest.size()) != 0) {
        return 0;
      }
      X509_STORE_CTX_set_error(store, X509_V_OK);
      return 1;
    } catch (...) {
      return 0;
    }
  }

  /**
   * @brief 为当前连接创建 TLS 1.2 及以上客户端上下文，安装 SPKI 验证回调。
   *
   * SSL_VERIFY_PEER 仍要求对端提供证书；VerifyPin 决定是否信任其公钥及服务器用途。
   * 教室内自签证书不依赖系统公网 CA 列表，不能以关闭验证代替 pin 校验。
   */
  websocketpp::lib::shared_ptr<TlsContext> MakeTls() {
    auto context = websocketpp::lib::make_shared<TlsContext>(TlsContext::tls_client);
    SSL_CTX_set_min_proto_version(context->native_handle(), TLS1_2_VERSION);
    SSL_CTX_set_options(context->native_handle(), SSL_OP_NO_COMPRESSION);
    SSL_CTX_set_verify(context->native_handle(), SSL_VERIFY_PEER, nullptr);
    SSL_CTX_set_cert_verify_callback(context->native_handle(), &VerifyPin, this);
    return context;
  }

  /**
   * @brief WebSocket open 回调中发送 hello，随后仍需收到 ready 才能发送业务音频。
   *
   * 此时 TLS 与 WebSocket 升级已经完成，连接握手不代表教学业务协议已经就绪。
   * hello 只携带设备 UUID 和固定教学口令，不向板端传递云服务凭据。
   */
  void SendHello(Client& client, Hdl handle) {
    // socket_init 时 TCP 还未打开。握手后再设置小发送缓冲和 TCP_NODELAY，
    // 避免内核排队数秒旧 PCM，也让短控制帧及时发出。
    websocketpp::lib::asio::error_code socket_error;
    auto& socket = client.get_con_from_hdl(handle)->get_raw_socket();
    socket.set_option(websocketpp::lib::asio::socket_base::send_buffer_size(4096),
                      socket_error);
    if (!socket_error) {
      socket.set_option(websocketpp::lib::asio::ip::tcp::no_delay(true), socket_error);
    }
    if (socket_error) {
      Fail("socket_setup");
      return;
    }
    const std::string hello = "{\"type\":\"hello\",\"device_id\":\"" + configured_.device_id +
                              "\",\"token\":\"" + kTeachingToken + "\",\"sample_rate\":16000}";
    websocketpp::lib::error_code error;
    client.send(handle, hello, websocketpp::frame::opcode::text, error);
    if (error) {
      Fail("hello_send");
    }
  }

  /**
   * @brief 网络回调先做无状态单帧解码，再交给 OnEvent 检查连接与轮次。
   *
   * 包括旧代消息也先经过格式检查：这里只忽略合法但迟到的旧回答，不容忍坏协议帧。
   * 解码异常在网络边界收束为 invalid_protocol，不让异常逃出 WebSocket++ 回调。
   */
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

  /**
   * @brief 注册当前连接使用的网络线程回调；所有回调由 ProcessConnection 的 poll 驱动。
   *
   * 关闭第三方原始日志后统一报告短阶段码；ping/pong 属于 WebSocket 控制层，
   * 无须让 application actor 理解心跳。仅匹配本端 ping 负载的 pong 才解除等待。
   */
  void Configure(Client& client) {
    client.clear_access_channels(websocketpp::log::alevel::all);
    client.clear_error_channels(websocketpp::log::elevel::all);
    client.init_asio();
    client.start_perpetual();
    client.set_tls_init_handler([this](Hdl) {
      return MakeTls();
    });
    client.set_open_handler([this, &client](Hdl handle) {
      SendHello(client, handle);
    });
    client.set_fail_handler([this](Hdl) {
      Fail("tls_connect");
    });
    client.set_close_handler([this](Hdl) {
      Fail("connection_closed");
    });
    client.set_pong_handler([this](Hdl, const std::string& payload) {
      if (payload == "boompi") {
        awaiting_pong_ = false;
      }
    });
    client.set_message_handler([this](Hdl, Client::message_ptr message) {
      OnMessage(message);
    });
  }

  /**
   * @brief 网络线程逐帧把自有队列交给 WebSocket++，同时检查两处发送滞留期限。
   *
   * 自有队列超时为 uplink_timeout，WebSocket 待发字节滞留为 send_timeout，均为 800 ms。
   * send 成功只表示传输层接收了字节，并非远端确认；已交给 socket 的旧帧不能撤回，
   * 后续 STOP/新代 START 和对端 generation 规则负责隔离它们。
   */
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

  /**
   * @brief 驱动单条连接的 I/O、发送节奏与超时，直到关闭或首个故障。
   *
   * 从连接尝试开始 5 秒内必须 ready；在线每隔 5 秒发 ping，再给 pong 最多 5 秒。
   * wait_for 会释放 mutex_，新 PCM、STOP 或关闭可唤醒；2 ms 周期也让收包及时被 poll。
   * @return 本次连接是否曾 ready，供外层决定是否重置重连退避，不表示退出时仍在线。
   */
  bool ProcessConnection(Client& client, Client::connection_ptr connection) {
    const auto started = Clock::now();
    auto ping_at = started;
    awaiting_pong_ = false;
    bool reached_ready = false;
    while (!stop_.load()) {
      // 1. 处理已到达的服务器消息，再发送应用排队的录音或 STOP。
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

  /**
   * @brief 在网络线程栈上创建一次 WSS 会话，用 /ws 路径连接发现或配置的端点。
   *
   * 44 字符 Base64 解码缓冲预留 33 字节，再取实际 SHA-256 的 32 字节作为 VerifyPin
   * 的比较目标。单消息最多 8192 字节，升级握手与业务 ready 各有受控超时路径。
   */
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

  /**
   * @brief 连接生命周期循环：清理旧轮次 → 准备网卡/端点 → 运行连接 → 退避重试。
   *
   * 退避从 500 ms 倍增到最多 8 秒，曾成功 ready 后回到最短等待；关闭可以打断等待。
   * 重连不会重放旧 PCM 或续接旧回答，新的 Online 由应用恢复空闲状态再接受提问。
   */
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
      changed_.wait_for(lock, backoff, [this] {
        return stop_.load();
      });
      backoff = std::min(backoff * 2, std::chrono::milliseconds(8000));
    }
  }

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
};

// 公共边界仅转发给 Impl，让应用头文件不暴露 WebSocket++、OpenSSL 和同步原语。
VoiceLink::VoiceLink() : impl_(std::make_unique<Impl>()) {}
VoiceLink::~VoiceLink() = default;

bool VoiceLink::Open(const config::VoiceClientConfig& settings) {
  return impl_->Open(settings);
}

bool VoiceLink::PollEvent(LinkEvent* event) {
  return impl_->PollEvent(event);
}

SendResult VoiceLink::SendAudio(std::uint32_t generation, const std::int16_t* pcm, bool start,
                                bool end, bool supersede) {
  return impl_->SendAudio(generation, pcm, start, end, supersede);
}

bool VoiceLink::Stop(std::uint32_t generation, bool retract) {
  return impl_->Stop(generation, retract);
}

void VoiceLink::Close() noexcept {
  impl_->Close();
}

}  // namespace boompi::network
