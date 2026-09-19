/** @file voice_net.cpp
 * @brief 独立网络任务：发现服务端 → WSS握手 → 收发BPV4 → 断线重连。
 * 应用投递控制与PCM，库持有发送队列；接收事件在短锁内交给应用。
 * 网络独占轮次/序号校验，系统网络服务负责地址、路由和Wi-Fi连接。
 */
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
#include <exception>
#include <mutex>
#include <thread>
#include <utility>
#include <websocketpp/client.hpp>
#include <websocketpp/config/asio_client.hpp>

#include "boompi/config/voice_client_config.h"
#include "network.h"
#include "voice_codec.h"

namespace voice_net
{

typedef std::chrono::steady_clock Clock;
typedef websocketpp::client<websocketpp::config::asio_tls_client> Client;
typedef websocketpp::connection_hdl Hdl;
typedef websocketpp::lib::asio::ssl::context TlsContext;

// 只跟踪本轮协议进度；DONE 不代表声卡已播完，尾播由音频线程判断。
enum class TurnPhase
{
    Retired,
    Uploading,
    WaitingReply,
    Complete
};
enum class ConnectionState
{
    Offline,
    Online,
    Failed
};

// 库会整批取走待发消息，get_buffered_amount不含当前async_write批次。
// 每批最多32帧；在途批次+待发队列合计最多约1.28s，同时容纳500ms句首突发。
static const std::size_t kQueueBytes = 32 * voice_codec::kFrameBytes;
// 接收队列还包含字幕和控制事件，不只计 PCM；溢出时整条连接失败以免掩盖缺帧。
static const std::size_t kEventCapacity = 64;
// 固定课堂 hello 口令，与只保存在服务端的 DashScope Key 无关。
static const char kTeachingToken[] = "boompi-teaching-shared-token-v1-2026";

// Open 在线程启动前写入，此后只读；thread_ 的创建、join 由应用生命周期串行控制。
static config::VoiceClientConfig configured_;
static std::thread thread_;
static std::atomic<bool> stop_{false};
// 除 stop_ 外，跨线程收发队列、连接状态和以下轮次字段都在 mutex_ 下访问。
static std::mutex mutex_;
static std::condition_variable changed_;
static Client::connection_ptr connection_;
// 库的get_buffered_amount本版本没有加锁；只在网络线程采样，应用在同一锁下扣容量。
static std::size_t buffered_bytes_{0};
static std::deque<LinkEvent> events_;
// TLS pin 与底层发送计时只在网络线程访问，不需要应用线程读取。
static std::array<unsigned char, 32> pin_{};
// 应用决定开始/取消；轮次号、上下行序号和旧结果隔离均由网络负责。
static std::uint32_t generation_{0};
static std::uint32_t uplink_sequence_{0};
static std::uint32_t downlink_sequence_{0};
static ConnectionState connection_state_{ConnectionState::Offline};
static TurnPhase turn_phase_{TurnPhase::Retired};
static bool audio_ended_{false};
static void AdvanceLocked(std::uint32_t generation, TurnPhase phase)
{
    generation_ = generation;
    turn_phase_ = phase;
    uplink_sequence_ = 0;
    downlink_sequence_ = 0;
    audio_ended_ = false;
    // 保留 generation 0 的连接状态通知；所有回答事件都属于已退休的业务代。
    events_.erase(std::remove_if(events_.begin(), events_.end(),
                                 [](const LinkEvent &event)
                                 {
                                     return event.generation != 0;
                                 }),
                  events_.end());
}

static void FailLocked(const char *code)
{
    if (connection_state_ == ConnectionState::Failed)
    {
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

static void Fail(const char *code)
{
    std::lock_guard<std::mutex> lock(mutex_);
    FailLocked(code);
}

static int VerifyPin_cb(X509_STORE_CTX *store, void *)
{
    X509 *certificate = X509_STORE_CTX_get0_cert(store);
    if (!certificate || X509_check_purpose(certificate, X509_PURPOSE_SSL_SERVER, 0) <= 0)
    {
        return 0;
    }
    X509_PUBKEY *key = X509_get_X509_PUBKEY(certificate);
    std::array<unsigned char, 32> digest{};
    unsigned int size = 0;
    // 摘要覆盖完整SPKI；X509_pubkey_digest只处理裸公钥，不能替代此处。
    if (!key ||
        ASN1_item_digest(ASN1_ITEM_rptr(X509_PUBKEY), EVP_sha256(), key, digest.data(),
                         &size) != 1 ||
        size != digest.size() || CRYPTO_memcmp(digest.data(), pin_.data(), digest.size()) != 0)
    {
        return 0;
    }
    X509_STORE_CTX_set_error(store, X509_V_OK);
    return 1;
}

static websocketpp::lib::shared_ptr<TlsContext> MakeTls_cb()
{
    auto context = websocketpp::lib::make_shared<TlsContext>(TlsContext::tls_client);
    if (SSL_CTX_set_min_proto_version(context->native_handle(), TLS1_2_VERSION) != 1)
    {
        return nullptr;  // WebSocket++将空TLS上下文作为连接初始化失败。
    }
    SSL_CTX_set_options(context->native_handle(), SSL_OP_NO_COMPRESSION);
    SSL_CTX_set_verify(context->native_handle(), SSL_VERIFY_PEER, nullptr);
    SSL_CTX_set_cert_verify_callback(context->native_handle(), &VerifyPin_cb, nullptr);
    return context;
}

static void SendHello_cb(Client &client, Hdl handle)
{
    // socket_init 时 TCP 还未打开。握手后再设置小发送缓冲和 TCP_NODELAY，
    // 避免内核排队数秒旧 PCM，也让短控制帧及时发出。
    websocketpp::lib::asio::error_code socket_error;
    auto &socket = client.get_con_from_hdl(handle)->get_raw_socket();
    socket.set_option(websocketpp::lib::asio::socket_base::send_buffer_size(4096),
                      socket_error);
    if (!socket_error)
    {
        socket.set_option(websocketpp::lib::asio::ip::tcp::no_delay(true), socket_error);
    }
    if (socket_error)
    {
        Fail("socket_setup");
        return;
    }
    const std::string hello = "HELLO 4 16000 " + configured_.device_id + " " + kTeachingToken;
    websocketpp::lib::error_code error;
    client.send(handle, hello, websocketpp::frame::opcode::text, error);
    if (error)
    {
        Fail("hello_send");
    }
}

static void OnMessage_cb(Client::message_ptr message)
{
    LinkEvent event;
    bool valid = false;
    // 解码取得消息负载的所有权，普通格式错误通过返回值交给连接失败路径。
    if (message->get_opcode() == websocketpp::frame::opcode::text)
    {
        valid = voice_codec::DecodeText(std::move(message->get_raw_payload()), event);
    }
    else if (message->get_opcode() == websocketpp::frame::opcode::binary)
    {
        valid = voice_codec::DecodeAudio(std::move(message->get_raw_payload()), event);
    }
    if (!valid)
    {
        Fail("invalid_protocol");
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (connection_state_ == ConnectionState::Failed || stop_.load())
    {
        return;
    }
    // 握手、轮次、音频连续性在一个入口校验，错误统一结束连接。
    if (event.kind == LinkEventKind::Online)
    {
        if (connection_state_ == ConnectionState::Online)
        {
            FailLocked("duplicate_ready");
            return;
        }
        connection_state_ = ConnectionState::Online;
    }
    else
    {
        if (connection_state_ != ConnectionState::Online)
        {
            FailLocked("message_before_ready");
            return;
        }
        if (event.generation < generation_ ||
            (event.generation == generation_ && turn_phase_ == TurnPhase::Retired))
        {
            return;
        }
        const bool audio = event.kind == LinkEventKind::Audio;
        if (!generation_ || event.generation != generation_ ||
            turn_phase_ == TurnPhase::Complete ||
            (turn_phase_ == TurnPhase::Uploading && event.kind != LinkEventKind::Error) ||
            (audio && (audio_ended_ || event.sequence != downlink_sequence_)))
        {
            FailLocked("reply_order");
            return;
        }
        if (audio)
        {
            ++downlink_sequence_;
            audio_ended_ = event.data.size() < voice_codec::kPcmBytes;
        }
        else if (event.kind == LinkEventKind::Done || event.kind == LinkEventKind::Error)
        {
            turn_phase_ = TurnPhase::Complete;
        }
    }
    if (events_.size() == kEventCapacity)
    {
        FailLocked("inbound_overflow");
        return;
    }
    events_.push_back(std::move(event));
}

static void Configure(Client &client)
{
    client.clear_access_channels(websocketpp::log::alevel::all);
    client.clear_error_channels(websocketpp::log::elevel::all);
    client.init_asio();
    client.start_perpetual();
    client.set_tls_init_handler(
        [](Hdl)
        {
            return MakeTls_cb();
        });
    client.set_open_handler(
        [&client](Hdl handle)
        {
            SendHello_cb(client, handle);
        });
    client.set_fail_handler(
        [](Hdl)
        {
            Fail("tls_connect");
        });
    client.set_close_handler(
        [](Hdl)
        {
            Fail("connection_closed");
        });
    client.set_pong_timeout_handler(
        [](Hdl, const std::string &)
        {
            Fail("heartbeat_timeout");
        });
    client.set_message_handler(
        [](Hdl, Client::message_ptr message)
        {
            OnMessage_cb(message);
        });
}

static SendResult SendLocked(const void *bytes, std::size_t size,
                             websocketpp::frame::opcode::value opcode, bool critical = false)
{
    if (connection_state_ != ConnectionState::Online || !connection_ || stop_.load())
    {
        return SendResult::Disconnected;
    }
    if (buffered_bytes_ + size > kQueueBytes)
    {
        if (critical)
        {
            FailLocked("control_backpressure");  // 不能投递取消时断开，让服务端终止旧任务。
            return SendResult::Disconnected;
        }
        return SendResult::Backpressure;
    }
    if (connection_->send(bytes, size, opcode))
    {
        FailLocked("send_failed");
        return SendResult::Disconnected;
    }
    buffered_bytes_ += size;
    changed_.notify_one();
    return SendResult::Ok;
}
// START与CANCEL都退休上一轮；递增、投递、清旧事件只在这里执行。
static SendResult NewTurn(TurnPhase phase, bool retract)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (generation_ == UINT32_MAX)
    {
        FailLocked("generation_exhausted");
        return SendResult::Disconnected;
    }
    const auto generation = generation_ + 1;
    const std::string text = std::string(phase == TurnPhase::Uploading ? "START " : "CANCEL ") +
                             std::to_string(generation) + (retract ? " 1" : " 0");
    const auto result =
        SendLocked(text.data(), text.size(), websocketpp::frame::opcode::text, true);
    if (result == SendResult::Ok)
    {
        AdvanceLocked(generation, phase);
    }
    return result;
}

static void ProcessConnection(Client &client, Client::connection_ptr connection)
{
    // 网络线程推进收发，同时检查握手与心跳期限。
    const auto started = Clock::now();
    auto ping_at = started;
    while (!stop_.load())
    {
        // 1. 推进库自己的收发队列，不再转发一份自有PCM队列。
        client.poll();
        std::unique_lock<std::mutex> lock(mutex_);
        if (connection_state_ == ConnectionState::Failed)
        {
            break;
        }
        // 2. 检查握手与心跳；断线交给外层网络任务重连。
        const auto now = Clock::now();
        // 所有库I/O回调都在本线程；持锁时应用不能send，读取库缓冲计数不会数据竞争。
        buffered_bytes_ = connection->get_buffered_amount();
        if (connection_state_ != ConnectionState::Online &&
            now - started > std::chrono::seconds(5))
        {
            FailLocked("hello_timeout");
            break;
        }
        // pong超时交给WebSocket++；发ping间隔长于超时，不能覆盖尚未触发的定时器。
        if (connection_state_ == ConnectionState::Online &&
            now - ping_at >= std::chrono::seconds(10))
        {
            websocketpp::lib::error_code error;
            connection->ping("boompi", error);
            if (error)
            {
                FailLocked("heartbeat_send");
                break;
            }
            ping_at = now;
        }
        changed_.wait_for(lock, std::chrono::milliseconds(2));
    }
}

static void Connect()
{
    // 先把保存的Base64指纹解码为摘要字节，TLS验证回调随后用它核对服务器公钥。
    const auto &endpoint = configured_;
    std::array<unsigned char, 33> pin{};
    if (endpoint.server_spki_sha256.size() != 44 ||
        EVP_DecodeBlock(
            pin.data(),
            reinterpret_cast<const unsigned char *>(endpoint.server_spki_sha256.data()),
            44) != 33)
    {
        Fail("invalid_pin");
        return;
    }
    std::copy_n(pin.begin(), pin_.size(), pin_.begin());

    Client client;
    Configure(client);
    websocketpp::lib::error_code error;
    const std::string address =
        "wss://" + endpoint.server_ip + ":" + std::to_string(endpoint.server_port) + "/ws";
    const auto connection = client.get_connection(address, error);
    if (error)
    {
        Fail("invalid_endpoint");
        return;
    }
    connection->set_open_handshake_timeout(5000);
    connection->set_pong_timeout(5000);
    connection->set_max_message_size(8192);
    // 在锁内公布连接对象，主线程随后通过该对象投递有界发送消息。
    {
        std::lock_guard<std::mutex> lock(mutex_);
        connection_ = connection;
        buffered_bytes_ = 0;
    }
    try
    {
        // 系统路由决定网卡；库依次完成TCP、TLS、WebSocket握手。
        client.connect(connection);
        ProcessConnection(client, connection);
    }
    catch (const std::exception &)
    {
        // 库调用和内存分配可能抛异常，退出前仍需撤回发送入口并关闭socket。
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
}

static void NetworkTask()
{
    // 系统负责联网；每次循环依次发现服务端、运行连接、等待重试。
    while (!stop_.load())
    {
        // 1. 清理上一条连接的数据，旧问题不会在重连后重新发送。
        {
            std::lock_guard<std::mutex> lock(mutex_);
            connection_state_ = ConnectionState::Offline;
            AdvanceLocked(generation_, TurnPhase::Retired);
        }
        try
        {
            // 2. 更新已配对端点，再建立一条WSS连接；失败后下一轮重新发现。
            if (!network::find_server(configured_, stop_))
            {
                Fail("server_discovery");
            }
            else if (!stop_.load())
            {
                Connect();
            }
        }
        catch (const std::exception &)
        {
            Fail("network_worker");
        }
        // 3. 连接结束后稍等再试；关闭请求可以提前唤醒等待。
        std::unique_lock<std::mutex> lock(mutex_);
        changed_.wait_for(lock, std::chrono::seconds(1),
                          []
                          {
                              return stop_.load();
                          });
    }
}

bool open(const config::VoiceClientConfig &settings)
{
    if (thread_.joinable())
    {
        return false;
    }
    generation_ = 0;
    configured_ = settings;
    stop_.store(false);
    try
    {
        thread_ = std::thread(NetworkTask);
    }
    catch (const std::exception &)
    {
        return false;
    }
    return true;
}

bool online()
{
    std::lock_guard<std::mutex> lock(mutex_);
    return connection_state_ == ConnectionState::Online;
}
bool uploading()
{
    std::lock_guard<std::mutex> lock(mutex_);
    return turn_phase_ == TurnPhase::Uploading;
}
bool poll(LinkEvent &event)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (events_.empty())
    {
        return false;
    }
    event = std::move(events_.front());
    events_.pop_front();
    return true;
}

SendResult start(bool supersede)
{
    return NewTurn(TurnPhase::Uploading, supersede);
}

SendResult send(const audio::VoiceFrame16k &pcm)
{
    // 同一把锁保护轮次、序号与发送入口，当前帧在其中完成编码并交付库的队列。
    std::lock_guard<std::mutex> lock(mutex_);
    if (connection_state_ != ConnectionState::Online || stop_.load())
    {
        return SendResult::Disconnected;
    }
    if (turn_phase_ != TurnPhase::Uploading || uplink_sequence_ == UINT32_MAX)
    {
        FailLocked("invalid_uplink");
        return SendResult::Disconnected;
    }
    const auto bytes = voice_codec::EncodeAudio(generation_, uplink_sequence_, pcm);
    const auto result =
        SendLocked(bytes.data(), bytes.size(), websocketpp::frame::opcode::binary);
    if (result == SendResult::Ok)
    {
        ++uplink_sequence_;
    }
    return result;
}

SendResult end()
{
    // END关闭本轮上传并进入等待回复阶段，generation继续用于后续取消和回复过滤。
    std::lock_guard<std::mutex> lock(mutex_);
    if (connection_state_ != ConnectionState::Online || stop_.load())
    {
        return SendResult::Disconnected;
    }
    if (turn_phase_ != TurnPhase::Uploading || uplink_sequence_ == 0)
    {
        FailLocked("invalid_end");
        return SendResult::Disconnected;
    }
    const std::string text = "END " + std::to_string(generation_);
    const auto result = SendLocked(text.data(), text.size(), websocketpp::frame::opcode::text);
    if (result == SendResult::Ok)
    {
        turn_phase_ = TurnPhase::WaitingReply;
    }
    return result;
}

bool cancel(bool retract)
{
    return NewTurn(TurnPhase::Retired, retract) == SendResult::Ok;
}

void close()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_.store(true);
        changed_.notify_all();
    }
    if (thread_.joinable())
    {
        thread_.join();
    }
    std::lock_guard<std::mutex> lock(mutex_);
    connection_state_ = ConnectionState::Offline;
    turn_phase_ = TurnPhase::Retired;
    events_.clear();
}
}  // namespace voice_net
