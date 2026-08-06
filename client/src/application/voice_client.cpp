/// @file 对话状态机：只编排已经实现的音频与 WSS 能力，不承载底层算法。
#include "boompi/application/voice_client.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <utility>

#include "boompi/audio/audio_engine.h"
#include "boompi/network/network_bootstrap.h"
#include "boompi/network/voice_transport.h"
#include "boompi/ui/device_ui.h"

namespace boompi::application {
namespace {

using Clock = std::chrono::steady_clock;
using audio::AudioEngine;
using audio::AudioEngineConfig;
using audio::CaptureFrame;
using audio::CaptureResult;
using audio::QueueTtsResult;
using network::Control;
using network::ControlKind;
using network::Inbound;
using network::InboundKind;
using network::NetworkBootstrap;
using network::NetworkBootstrapResult;
using network::Pcm64Header;
using network::SendOutcome;
using network::TransportConfig;
using network::VoiceTransport;
using network::WireIds;
using ui::DeviceUiAction;
using ui::DeviceUiState;
constexpr std::size_t kPreRollFrames = 25U;
constexpr unsigned kBargeCandidateFrames = 6U;
constexpr unsigned kBargeReferenceLowFrames = 3U;
constexpr unsigned kBargeReferenceWaitFrames = 15U;
constexpr unsigned kBargeEchoClearFrames = 3U;
constexpr unsigned kBargeConfirmFrames = 3U;
constexpr unsigned kBargeTailFreezeFrames = 10U;
constexpr unsigned kBargeRetryCooldownFrames = 15U;
constexpr std::size_t kAudioFrameMs = 20U;
constexpr int kCancelAckWaitMs = 3000;
constexpr std::size_t kMaximumBargeProbeFrames =
    kBargeCandidateFrames + kBargeReferenceWaitFrames +
    kBargeEchoClearFrames + kBargeConfirmFrames;
constexpr std::size_t kCancelAckFrames =
    (static_cast<std::size_t>(kCancelAckWaitMs) + kAudioFrameMs - 1U) /
    kAudioFrameMs;
// cancel 发出时 capture actor 还可能持有完整 4 槽队列；容量必须覆盖这些已采帧、
// 最慢 27 帧探针和整个 3 s ACK 窗，不能在合法 deadline 前先丢掉打断语音。
constexpr std::size_t kCaptureQueueSlackFrames = 4U;
constexpr std::size_t kBargeBufferFrames =
    kMaximumBargeProbeFrames + kCancelAckFrames + kCaptureQueueSlackFrames;
constexpr std::uint32_t kMaximumTurnFrames = 3000U;  // 60 s：单次用户语音上限。
/// Host harness 缩短等待但保留各 deadline 的相对顺序；板端始终采用 production_ms。
constexpr auto TestableDuration([[maybe_unused]] const int production_ms,
                                [[maybe_unused]] const int harness_ms) {
#if defined(BOOMPI_VOICE_CLIENT_HARNESS)
  return std::chrono::milliseconds(harness_ms);
#else
  return std::chrono::milliseconds(production_ms);
#endif
}
constexpr auto kConnectWait = TestableDuration(6000, 30);
constexpr auto kHelloAckWait = TestableDuration(5000, 30);
constexpr auto kHeartbeatWait = TestableDuration(30000, 600);
constexpr auto kSpeechStartWait = TestableDuration(6000, 250);
constexpr auto kMaximumTurnWait = TestableDuration(60000, 250);
constexpr auto kFirstResponseHint = TestableDuration(15000, 15);
constexpr auto kResponseWait = TestableDuration(30000, 300);
constexpr auto kCancelAckWait = TestableDuration(kCancelAckWaitMs, 250);
constexpr auto kDrainPlaybackWait = TestableDuration(30000, 20);
constexpr auto kFollowUpWait = TestableDuration(3000, 30);
// 追问不经过唤醒词，因此准入条件必须比播放期间的打断更严格。
// 后端先用 300 ms 尾音保护清掉已知渲染尾音，随后还需连续 400 ms
// 判定为近讲；500 ms pre-roll 仍保留真实发言的句首。
constexpr unsigned kFollowUpNearFrames = 20U;
constexpr unsigned kEventsPerCaptureFrame = 8U;
constexpr auto kActorPollInterval = std::chrono::milliseconds(100);

const char* QueueTtsResultName(const QueueTtsResult result) noexcept {
  switch (result) {
    case QueueTtsResult::kQueued: return "queued";
    case QueueTtsResult::kNotOpen: return "not_open";
    case QueueTtsResult::kInvalidArgument: return "invalid_argument";
    case QueueTtsResult::kNotActive: return "not_active";
    case QueueTtsResult::kEnding: return "ending";
    case QueueTtsResult::kFull: return "full";
    case QueueTtsResult::kDiscontinuous: return "discontinuous";
  }
  return "unknown";
}

/** WSS 线程单生产、主 actor 单消费；固定 64 项，溢出时主动断线而不是覆盖事件。 */
class EventQueue final {
 public:
  bool Push(Inbound event) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (count_ == slots_.size()) {
      overflow_ = true;
      return false;
    }
    slots_[tail_] = std::move(event);
    tail_ = (tail_ + 1U) % slots_.size();
    ++count_;
    return true;
  }
  bool Pop(Inbound* event) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (count_ == 0U) return false;
    *event = std::move(slots_[head_]);
    head_ = (head_ + 1U) % slots_.size();
    --count_;
    return true;
  }
  bool overflowed() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return overflow_;
  }
  void Clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    // 重连是冷路径；主动释放每个槽残留的文本和音频容量。
    for (auto& slot : slots_) slot = {};
    head_ = tail_ = count_ = 0U;
    overflow_ = false;
  }
 private:
  mutable std::mutex mutex_;
  std::array<Inbound, 64U> slots_{};
  std::size_t head_{0U};
  std::size_t tail_{0U};
  std::size_t count_{0U};
  bool overflow_{false};
};

/// 一轮对话的完整状态。所有转换只发生在 application actor 线程。
enum class State : std::uint8_t {
  kWaitingForWake,
  kWaitingForSpeech,
  kCapturingSpeech,
  kWaitingForResponse,
  kPlayingResponse,
  kCancelPending,
  kDrainingPlayback,
  kWaitingForFollowUp,
};
enum class BargeProbe : std::uint8_t {
  kIdle,
  kWaitReferenceLow,
  kClear,
  kVerify,
};

class VoiceClient final {
 public:
  VoiceClient(const config::VoiceClientConfig& config, std::string* error)
      : config_(config),
        endpoint_{config.server_ip, config.server_port,
                  config.server_spki_sha256},
        error_(error) {}
  ~VoiceClient() {
    stop_network_.store(true, std::memory_order_release);
    if (transport_) transport_->Close();
    audio_.Close();
    if (network_thread_.joinable()) network_thread_.join();
  }

  bool Run(const volatile std::sig_atomic_t* stop) {
    // 主线程就是 application actor。Capture 每 20 ms 返回一次，因此连接完成、网络事件
    // 和状态 deadline 都在同一串行时序中处理，不需要给会话字段再加锁。
    AudioEngineConfig audio_config{};
    audio_config.capture_pcm = config_.capture_pcm;
    audio_config.playback_pcm = config_.playback_pcm;
    audio_config.snowboy_resource = config_.snowboy_resource_path;
    audio_config.snowboy_model = config_.snowboy_model_path;
    audio_config.snowboy_sensitivity = config_.snowboy_sensitivity;
    audio_config.vad_min_dbfs = config_.vad_min_dbfs;
    volume_ = ui::DeviceUi::LoadVolume(config_.volume_percent);
    audio_config.playback_gain = static_cast<float>(volume_) *
                                 config_.speaker_gain_percent / 10000.0F;
    audio_config.left_polarity = config_.capture_left_polarity;
    audio_config.right_polarity = config_.capture_right_polarity;
    if (!ui_.Open()) std::cerr << "boompi-client: display unavailable; continuing voice-only\n";
    ui_.SetVolume(volume_);
    ui_.SetState(DeviceUiState::kOffline);
    if (!audio_.Open(audio_config)) return Fail(audio_.last_error());
    try { network_thread_ = std::thread(&VoiceClient::NetworkLoop, this); }
    catch (...) { return Fail("network thread creation failed"); }
    next_connect_ = Clock::now();
    std::cout << "boompi-client: voice loop ready; wake_word=snowboy"
              << "; wake_sensitivity=" << config_.snowboy_sensitivity
              << "; vad_min_dbfs=" << config_.vad_min_dbfs
              << "; barge_min_dbfs=" << config_.barge_min_dbfs << std::endl;
    while (stop == nullptr || *stop == 0) {
      if (!transport_ && endpoint_ready_.load(std::memory_order_acquire) &&
          Clock::now() >= next_connect_) StartConnect();
      if (listener_failed_) return Fail(audio_.last_error());
      CaptureFrame frame{};
      const CaptureResult capture =
          audio_.Capture(&frame, kActorPollInterval);
      FinishConnect();
      // 到期状态必须先封口；否则刚过 deadline 才入队的 ACK/响应会因本轮先消费
      // 网络事件而重写 deadline，使同一时序随 Capture 返回时刻产生不同结果。
      const bool frame_still_current = AdvanceDeadlines(Clock::now());
      if (listener_failed_) return Fail(audio_.last_error());
      DrainEvents();
      PollUi();
      if (listener_failed_) return Fail(audio_.last_error());
      if (events_.overflowed()) LoseConnection("network event queue overflow");
      if (capture == CaptureResult::kFailed) return Fail(audio_.last_error());
      if (capture == CaptureResult::kTimeout) continue;
      if (!frame_still_current) {
        // Deadline/drain transitions may intentionally reject the frame that
        // was already dequeued above.  It still belongs to the continuous
        // capture timeline; consume its sequence here so the following real
        // frame is not misreported as an ALSA/actor discontinuity.
        next_capture_sequence_ = frame.sequence + 1U;
        continue;
      }
      OnFrame(frame);
      if (listener_failed_) return Fail(audio_.last_error());
    }
    return true;
  }

 private:
  bool Fail(std::string why) {
    ui_.SetState(DeviceUiState::kError);
    if (error_ != nullptr) *error_ = std::move(why);
    return false;
  }
  bool SendControl(ControlKind kind, WireIds ids) {
    Control control{};
    control.kind = kind;
    control.message_id = "client-" + std::to_string(message_++);
    control.ids = ids;
    return transport_ && transport_->SendControl(control);
  }
  static bool SameTurn(const WireIds& left, const WireIds& right) {
    return left.session == right.session && left.turn == right.turn &&
           left.epoch == right.epoch;
  }
  static bool SameIds(const WireIds& left, const WireIds& right) {
    return SameTurn(left, right) && left.stream == right.stream;
  }
  bool IsStale(const WireIds& ids) const {
    return turn_ids_.session != 0U && ids.turn != 0U &&
           ids.session == turn_ids_.session &&
           ids.epoch != 0U && ids.epoch < turn_ids_.epoch;
  }
  bool IsCurrentResponse(const Inbound& event) const {
    return response_ids_.session != 0U && SameIds(event.ids, response_ids_);
  }
  void ProtocolViolation(const char* reason) {
    std::cerr << "boompi-client: protocol error; reason=" << reason << std::endl;
    LoseConnection(reason);
  }
  void ResetResponse() {
    response_ids_ = {};
    response_id_.clear();
    downlink_sequence_ = 0U;
    response_audio_started_ = false;
    response_audio_ended_ = false;
  }
  void ResetListener() {
    if (!audio_.ResetListener())
      listener_failed_ = true;
  }
  static std::size_t Utf8Cut(std::string_view text, std::size_t limit) {
    std::size_t end = std::min(text.size(), limit);
    if (end != text.size()) while (end != 0U &&
        (static_cast<unsigned char>(text[end]) & 0xC0U) == 0x80U) --end;
    return end;
  }
  void ShowText(const std::string& delta) {
    subtitle_ += delta;
    if (subtitle_.size() > 126U) {
      subtitle_.erase(0U, subtitle_.size() - 126U);
      while (!subtitle_.empty() &&
             (static_cast<unsigned char>(subtitle_.front()) & 0xC0U) == 0x80U)
        subtitle_.erase(0U, 1U);
    }
    const std::size_t first = Utf8Cut(subtitle_, 63U);
    const std::size_t second = Utf8Cut(std::string_view(subtitle_).substr(first), 63U);
    ui_.SetText(std::string_view(subtitle_).substr(0U, first),
                std::string_view(subtitle_).substr(first, second));
  }
  void ClearSubtitle() {
    subtitle_.clear();
    ui_.SetText({}, {});
  }
  void MarkFirstResponseProgress() {
    await_hint_pending_ = false;
    if (!await_hint_visible_) return;
    // response.start 只是云端占位，不能清除 15 s 提示。真正的文本、音频或
    // 终止事件到达后再清理，避免用户在模型仍无输出时看不到进度反馈。
    await_hint_visible_ = false;
    ClearSubtitle();
  }
  void ApplyVolume(const std::uint8_t percent) {
    volume_ = std::min<std::uint8_t>(percent, 100U);
    audio_.SetPlaybackGain(static_cast<float>(volume_) *
                           config_.speaker_gain_percent / 10000.0F);
    ui_.SetVolume(volume_);
  }

  void PollUi() {
    std::uint8_t selected_volume = volume_;
    if (ui_.PollVolumeChange(&selected_volume))
      ApplyVolume(selected_volume);

    DeviceUiAction action{};
    if (!ui_.PollAction(&action)) return;
    if (action == DeviceUiAction::kWake) {
      if (state_ == State::kWaitingForWake && turn_ids_.session != 0U)
        manual_wake_pending_ = true;
      else if (turn_ids_.session == 0U)
        std::cout << "boompi-client: touch wake ignored; server offline" << std::endl;
      return;
    }
    if (action == DeviceUiAction::kInterrupt &&
        (state_ == State::kPlayingResponse ||
         state_ == State::kDrainingPlayback)) {
      ClearSubtitle();
      // response.done 只表示服务端已发完；ALSA 尾音未播完时仍需通知服务端
      // 丢弃完整 assistant turn，避免把用户没有听到的内容留进上下文。
      AbortCurrent("touch interrupt cancel failed");
    }
  }

  bool AdvanceDeadlines(const Clock::time_point now) {
    if (turn_ids_.session == 0U) {
      if (!transport_ || now < state_deadline_) return true;
      LoseConnection("connect or hello timeout");
      return false;
    }
    if (now >= heartbeat_deadline_) {
      LoseConnection("server heartbeat timed out");
      return false;
    }
    if (state_ == State::kDrainingPlayback && audio_.playback_done()) {
      if (audio_.playback_failed()) listener_failed_ = true;
      else FinishTurn(true);
      return false;
    }
    if (state_ != State::kWaitingForWake && now >= state_deadline_) {
      if (state_ == State::kWaitingForSpeech ||
          state_ == State::kWaitingForFollowUp) {
        state_ = State::kWaitingForWake;
        pre_head_ = pre_count_ = 0U;
        ui_.SetState(DeviceUiState::kIdle);
        ResetListener();
      } else if (state_ == State::kCancelPending) {
        LoseConnection("voice cancel timed out");
      } else if (state_ == State::kDrainingPlayback) {
        AbortCurrent("drain timeout cancel failed");
      } else {
        AbortCurrent("voice timeout cancel failed");
      }
      return false;
    }
    if (state_ == State::kWaitingForResponse && await_hint_pending_ &&
        now >= await_hint_at_) {
      await_hint_pending_ = false;
      await_hint_visible_ = true;
      ui_.SetText("还在思考", "云端正在组织回答，请稍候");
      std::cout << "boompi-client: first response pending 15s" << std::endl;
    }
    return true;
  }

  void ResetBargeProbe() {
    barge_probe_frames_ = barge_cooldown_frames_ = 0U;
    barge_probe_ = BargeProbe::kIdle;
    audio_.SetPlaybackScale(1.0F);
  }
  void ResetBargeTurn() {
    barge_ended_ = false;
    barge_turn_admitted_ = false;
    barge_buffer_overflow_ = false;
    barge_quiet_frames_ = 0U;
    barge_silence_frames_ = 0U;
  }
  void RejectBargeProbe() {
    std::cout << "boompi-client: barge-in probe rejected" << std::endl;
    ResetBargeProbe();
    ResetBargeTurn();
    barge_cooldown_frames_ = kBargeRetryCooldownFrames;
  }

  void BeginListening(const char* reason) {
    state_ = State::kWaitingForSpeech;
    state_deadline_ = Clock::now() + kSpeechStartWait;
    ui_.SetState(DeviceUiState::kListening);
    std::cout << "boompi-client: " << reason << std::endl;
    ResetListener();
  }

  void NetworkLoop() {
    NetworkBootstrapResult configured{
        config_.server_ip, config_.server_port, config_.server_spki_sha256};
    const NetworkBootstrapResult* const explicit_server =
        configured.host.empty() ? nullptr : &configured;
    while (!stop_network_.load(std::memory_order_acquire)) {
      if (!network_needed_.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        continue;
      }
      NetworkBootstrapResult found{};
      if (NetworkBootstrap::Start(explicit_server, &found, &stop_network_)) {
        std::lock_guard<std::mutex> lock(network_mutex_);
        endpoint_ = std::move(found);
        endpoint_ready_.store(true, std::memory_order_release);
        network_needed_.store(false, std::memory_order_release);
      }
      for (unsigned i = 0U; i != 20U && !stop_network_.load(std::memory_order_acquire); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  }

  // Connect 只启动 WSS/ASIO 线程；hello 必须等 open callback 确认 connected 后由 actor 发送。
  void StartConnect() {
    try {
      transport_ = std::make_unique<VoiceTransport>();
      hello_sent_ = false;
      state_deadline_ = Clock::now() + kConnectWait;
      TransportConfig tc{};
      tc.device_id = config_.device_id;
      {
        std::lock_guard<std::mutex> lock(network_mutex_);
        tc.host = endpoint_.host;
        tc.port = endpoint_.port;
        tc.spki_sha256_base64 = endpoint_.spki_sha256_base64;
      }
      if (!transport_->Connect(std::move(tc), [this](Inbound e) { events_.Push(std::move(e)); },
          [this](std::string why) {
            Inbound e{};
            e.kind = InboundKind::kError;
            e.error_code = "transport";
            e.text = std::move(why);
            events_.Push(std::move(e));
          }))
        LoseConnection("connection startup failed");
    } catch (...) {
      LoseConnection("connection startup failed");
    }
  }

  void FinishConnect() {
    if (!transport_ || hello_sent_ || !transport_->connected()) return;
    if (!SendControl(ControlKind::kHello, {})) {
      LoseConnection("hello send failed");
    } else {
      hello_sent_ = true;
      state_deadline_ = Clock::now() + kHelloAckWait;
    }
  }

  // 连接错误会使当前 turn 失效。旧事件、旧音频和旧 epoch 必须一起清掉，不能重传。
  void LoseConnection(const char* reason) {
    if (transport_) transport_->Close();
    transport_.reset();
    events_.Clear();
    audio_.DropPlayback();
    ClearSubtitle();
    await_hint_pending_ = await_hint_visible_ = false;
    ui_.SetState(DeviceUiState::kOffline);
    state_ = State::kWaitingForWake;
    turn_ids_ = {};
    session_epoch_ = 0U;
    ResetResponse();
    pre_head_ = pre_count_ = 0U;
    ResetBargeProbe();
    ResetBargeTurn();
    cancel_started_at_ = {};
    finish_after_cancel_ack_ = false;
    manual_wake_pending_ = false;
    ResetListener();
    const unsigned seconds = reconnect_attempt_ < 5U ? 1U << reconnect_attempt_ : 30U;
    if (reconnect_attempt_ < 5U) ++reconnect_attempt_;
    next_connect_ = Clock::now() + std::chrono::seconds(seconds);
    network_needed_.store(true, std::memory_order_release);
    std::cerr << "boompi-client: network retry in " << seconds << "s; reason=" << reason << std::endl;
  }

  void DrainEvents() {
    Inbound event{};
    // capture 环只有 4 × 20 ms（80 ms）缓冲；限制单帧事件消费量，避免持续下行饿死采集 actor。
    for (unsigned handled = 0U;
         handled < kEventsPerCaptureFrame && transport_ && !listener_failed_ &&
         events_.Pop(&event);
         ++handled) {
      HandleEvent(std::move(event));
    }
  }

  // 本地音频连续性/容量故障只取消当前轮次；WSS/TLS 仍健康时不能丢掉持久 provider 会话。
  void AbortCurrent(const char* failure) {
    audio_.DropPlayback();
    ResetBargeProbe();
    ResetBargeTurn();
    finish_after_cancel_ack_ = true;
    const bool has_response = response_ids_.session != 0U;
    if (!SendControl(has_response ? ControlKind::kResponseCancel : ControlKind::kTurnCancel,
                     has_response ? response_ids_ : turn_ids_)) {
      LoseConnection(failure);
      return;
    }
    state_ = State::kCancelPending;
    cancel_started_at_ = Clock::now();
    state_deadline_ = cancel_started_at_ + kCancelAckWait;
  }

  // WSS 回调只生产“已解码的单帧”。turn/epoch/response/sequence 都由
  // 这个 application actor 唯一判定，因此不需要跨线程的协议状态锁。
  void HandleEvent(Inbound event) {
    if (event.kind == InboundKind::kError && event.error_code == "transport") {
      MarkFirstResponseProgress();
      LoseConnection("transport error");
      return;
    }
    if (event.kind == InboundKind::kHeartbeat) {
      heartbeat_deadline_ = Clock::now() + kHeartbeatWait;
      return;
    }
    if (event.kind == InboundKind::kHelloAck) {
      if (!hello_sent_ || turn_ids_.session != 0U || event.ids.session == 0U ||
          event.ids.turn != 0U || event.ids.stream != 0U || event.ids.epoch == 0U) {
        ProtocolViolation("invalid hello.ack identity");
        return;
      }
      session_epoch_ = event.ids.epoch;
      turn_ids_ = {event.ids.session, 1U, 1U, event.ids.epoch};
      reconnect_attempt_ = 0U;
      heartbeat_deadline_ = Clock::now() + kHeartbeatWait;
      pre_head_ = pre_count_ = 0U;
      ResetResponse();
      ResetListener();
      ui_.SetState(DeviceUiState::kIdle);
      std::cout << "boompi-client: secure session ready" << std::endl;
      return;
    }
    if (turn_ids_.session == 0U) {
      ProtocolViolation("event received before hello.ack");
      return;
    }
    // 正常结束会推进 epoch。旧 epoch 的延迟帧只能丢弃，不得影响新 turn。
    if (IsStale(event.ids)) return;

    if (event.kind == InboundKind::kError) {
      MarkFirstResponseProgress();
      std::cerr << "boompi-client: server error code=" << event.error_code << std::endl;
      const bool session_error = event.ids.session == turn_ids_.session &&
          event.ids.turn == 0U && event.ids.stream == 0U &&
          event.ids.epoch == session_epoch_;
      const bool turn_error = SameTurn(event.ids, turn_ids_) &&
          event.ids.stream != 0U;
      if (!session_error && !turn_error) {
        ProtocolViolation("wrong error identity");
      } else if (session_error) {
        LoseConnection("session error");
      } else if (state_ == State::kCancelPending) {
        CompleteCancel(false);
      } else {
        FinishTurn(false);
      }
      return;
    }

    if (state_ == State::kCancelPending) {
      // cancel 发出后可能与 response.start/done 交叉。仍记住新出现的
      // response 身份，但不再向 UI/播放器发布它的内容。
      if (event.kind == InboundKind::kResponseStart &&
          response_ids_.session == 0U && SameTurn(event.ids, turn_ids_) &&
          event.ids.stream != 0U) {
        response_ids_ = event.ids;
        response_id_ = std::move(event.response_id);
        return;
      }
      if (event.kind == InboundKind::kCancelled) {
        const bool stream_matches = response_ids_.session == 0U
            ? event.ids.stream != 0U
            : event.ids.stream == response_ids_.stream;
        if (!SameTurn(event.ids, turn_ids_) || !stream_matches) {
          ProtocolViolation("wrong cancellation identity");
          return;
        }
        MarkFirstResponseProgress();
        CompleteCancel(true);
        return;
      }
      if (event.kind == InboundKind::kDone && IsCurrentResponse(event) &&
          event.response_id == response_id_) return;
      if (response_ids_.session != 0U && IsCurrentResponse(event)) return;
      ProtocolViolation("wrong event while cancellation is pending");
      return;
    }

    if (event.kind == InboundKind::kCancelled) {
      ProtocolViolation("unexpected response.cancelled");
      return;
    }
    if (state_ == State::kDrainingPlayback) {
      if (IsCurrentResponse(event)) return;
      ProtocolViolation("wrong event after response.done");
      return;
    }

    state_deadline_ = Clock::now() + kResponseWait;
    switch (event.kind) {
      case InboundKind::kResponseStart:
        if (state_ != State::kWaitingForResponse ||
            response_ids_.session != 0U || !SameTurn(event.ids, turn_ids_) ||
            event.ids.stream == 0U) {
          ProtocolViolation("wrong response.start identity or order");
          return;
        }
        response_ids_ = event.ids;
        response_id_ = std::move(event.response_id);
        downlink_sequence_ = 0U;
        response_audio_started_ = false;
        response_audio_ended_ = false;
        if (!await_hint_visible_) ClearSubtitle();
        break;
      case InboundKind::kTextDelta:
        if (!IsCurrentResponse(event) || event.response_id != response_id_) {
          ProtocolViolation("wrong response.text_delta identity");
          return;
        }
        MarkFirstResponseProgress();
        ShowText(event.text);
        break;
      case InboundKind::kAudioStart:
        if (!IsCurrentResponse(event) || event.response_id != response_id_ ||
            response_audio_started_) {
          ProtocolViolation("invalid response.audio_start");
          return;
        }
        MarkFirstResponseProgress();
        if (!audio_.BeginPlayback()) {
          LoseConnection("playback start failed");
          break;
        }
        response_audio_started_ = true;
        state_ = State::kPlayingResponse;
        ui_.SetState(DeviceUiState::kSpeaking);
        ResetBargeProbe();
        break;
      case InboundKind::kPcm: {
        if (!IsCurrentResponse(event) || !response_audio_started_ ||
            response_audio_ended_ || event.pcm.sequence != downlink_sequence_ ||
            ((event.pcm.sequence == 0U) != ((event.pcm.flags & 1U) != 0U))) {
          ProtocolViolation("PCM identity or sequence mismatch");
          return;
        }
        if ((event.pcm.flags & 4U) != 0U) {
          AbortCurrent("discontinuous response cancel failed");
          return;
        }
        MarkFirstResponseProgress();
        const QueueTtsResult queued = audio_.QueueTts24k(
            event.audio.data(), event.audio_size, event.pcm.sequence);
        if (queued != QueueTtsResult::kQueued) {
          std::cerr << "boompi-client: TTS queue rejected; reason="
                    << QueueTtsResultName(queued) << std::endl;
          AbortCurrent("TTS response cancel failed");
          return;
        }
        ++downlink_sequence_;
        response_audio_ended_ = (event.pcm.flags & 2U) != 0U;
        break;
      }
      case InboundKind::kDone:
        if (!IsCurrentResponse(event) || event.response_id != response_id_ ||
            (response_audio_started_ && !response_audio_ended_)) {
          ProtocolViolation("invalid response.done");
          return;
        }
        MarkFirstResponseProgress();
        if (audio_.playback_active() && !audio_.EndPlayback())
          LoseConnection("playback finish failed");
        // response.done 只表示下发结束；ALSA drain 完成前仍保留触屏/语音打断入口。
        else {
          state_ = State::kDrainingPlayback;
          state_deadline_ = Clock::now() + kDrainPlaybackWait;
          ui_.SetState(DeviceUiState::kSpeakingTail);
        }
        break;
      default: break;
    }
  }

  void AdvanceTurn() {
    ++turn_ids_.turn;
    ++turn_ids_.stream;
    ++turn_ids_.epoch;
    ResetResponse();
  }

  // 正常结束和异常取消都推进 epoch 并进入 follow-up；异常路径还会立即丢弃残余播放。
  void FinishTurn(bool normal) {
    ResetBargeProbe();
    ResetBargeTurn();
    follow_up_near_frames_ = 0U;
    finish_after_cancel_ack_ = false;
    await_hint_pending_ = await_hint_visible_ = false;
    if (!normal) {
      audio_.DropPlayback();
      ClearSubtitle();
    }
    AdvanceTurn();
    ResetListener();
    state_ = State::kWaitingForFollowUp;
    state_deadline_ = Clock::now() + kFollowUpWait;
    ui_.SetState(DeviceUiState::kHappy);
  }

  void ResumeAfterBarge() {
    // 取消播放和准入新用户轮次是两个独立决策；不能仅因 cancel ACK
    // 已到达，就把静音期间的播放残留上传为用户语音。
    if (!barge_turn_admitted_) {
      FinishTurn(false);
      return;
    }
    const bool utterance_ended = barge_ended_;
    barge_turn_admitted_ = false;
    barge_ended_ = false;
    audio_.DropPlayback();
    finish_after_cancel_ack_ = false;
    AdvanceTurn();
    StartTurn(utterance_ended);
  }

  void CompleteCancel(const bool acknowledged) {
    if (acknowledged && cancel_started_at_ != Clock::time_point{}) {
      const auto wait = std::chrono::duration_cast<std::chrono::milliseconds>(
          Clock::now() - cancel_started_at_);
      std::cout << "boompi-client: cancel ACK wait_ms=" << wait.count()
                << std::endl;
    }
    cancel_started_at_ = {};
    if (finish_after_cancel_ack_) {
      FinishTurn(false);
      return;
    }
    if (barge_turn_admitted_ && !barge_buffer_overflow_) {
      ResumeAfterBarge();
      return;
    }
    if (barge_buffer_overflow_)
      std::cerr << "boompi-client: barge-in buffer overflow; utterance discarded"
                << std::endl;
    // 未经语音确认的取消（例如触屏）不创建空 turn。
    pre_head_ = pre_count_ = 0U;
    FinishTurn(false);
  }

  // pre-roll 是 25 个固定槽的滚动窗；保存的是 AEC 后 PCM，发送时仍校验 capture sequence。
  void SavePreRoll(const CaptureFrame& frame) {
    if (state_ == State::kCancelPending && barge_turn_admitted_ &&
        pre_count_ == pre_.size()) {
      barge_buffer_overflow_ = true;
      return;
    }
    pre_[(pre_head_ + pre_count_) % pre_.size()] = frame;
    const bool preserve_barge = barge_probe_ != BargeProbe::kIdle ||
        (state_ == State::kCancelPending && barge_turn_admitted_);
    const std::size_t limit = preserve_barge ? pre_.size() : kPreRollFrames;
    if (pre_count_ < limit) ++pre_count_;
    else pre_head_ = (pre_head_ + 1U) % pre_.size();
  }

  void KeepNewestPreRoll(const std::size_t count) {
    const std::size_t keep = std::min(count, pre_count_);
    pre_head_ = (pre_head_ + pre_count_ - keep) % pre_.size();
    pre_count_ = keep;
  }

  bool IsBargeVoice(const CaptureFrame& frame) const {
    return frame.near_voice && frame.voice_dbfs >= config_.barge_min_dbfs;
  }

  // 打断已在播放探针中确认；等 cancel ACK 时只跟踪同一句的结尾。
  void TrackBargeUtterance(const CaptureFrame& frame) {
    if (frame.near_voice) barge_silence_frames_ = 0U;
    else if (barge_silence_frames_ < kBargeTailFreezeFrames)
      ++barge_silence_frames_;
    if (frame.vad_ended) barge_ended_ = true;
  }

  SendOutcome SendAudio(const CaptureFrame& frame, bool end) {
    Pcm64Header h{};
    h.flags = static_cast<std::uint16_t>(
        (uplink_sequence_ == 0U ? 1U : 0U) | (end ? 2U : 0U));
    h.sequence = uplink_sequence_;
    h.timestamp_us = frame.timestamp_us;
    h.ids = turn_ids_;
    const SendOutcome outcome = transport_->SendPcm64(
        h, reinterpret_cast<const std::uint8_t*>(frame.pcm.data()),
        frame.pcm.size() * sizeof(frame.pcm[0]));
    if (outcome == SendOutcome::kOk) ++uplink_sequence_;
    return outcome;
  }

  // 背压只取消当前 turn；连接/协议错误才重建持久 WSS 会话。
  bool SendAudioOrRecover(const CaptureFrame& frame, bool end,
                          const char* rejected_reason) {
    const SendOutcome outcome = SendAudio(frame, end);
    if (outcome == SendOutcome::kOk) return true;
    if (outcome == SendOutcome::kBackpressure) {
      std::cout << "boompi-client: uplink backpressure; aborting turn" << std::endl;
      AbortCurrent("uplink backpressure cancel failed");
    } else {
      LoseConnection(rejected_reason);
    }
    return false;
  }

  void WaitForResponse() {
    state_ = State::kWaitingForResponse;
    state_deadline_ = Clock::now() + kResponseWait;
    await_hint_at_ = Clock::now() + kFirstResponseHint;
    await_hint_pending_ = true;
    await_hint_visible_ = false;
    ui_.SetState(DeviceUiState::kThinking);
    std::cout << "boompi-client: VAD speech ended; voice turn committed" << std::endl;
  }

  // turn.start 成功后先发送最老的 pre-roll，再发送实时帧。若打断句在
  // cancel ACK 前已结束，最后一条冻结帧同时携带 END，不把 ACK 后的静音当成用户语音。
  bool StartTurn(const bool buffered_utterance_ended = false) {
    const std::size_t buffered_frames = pre_count_;
    if (buffered_utterance_ended && buffered_frames == 0U) {
      // 先拒绝本地不完整输入，不能在 wire 上留下没有 PCM/commit 的孤立 turn。
      LoseConnection("empty buffered barge turn");
      return false;
    }
    if (turn_ids_.session == 0U ||
        !SendControl(ControlKind::kTurnStart, turn_ids_)) {
      LoseConnection("turn.start failed");
      return false;
    }
    uplink_sequence_ = 0U;
    state_ = State::kCapturingSpeech;
    // 帧数上限保证有效音频不超 60 s；墙钟上限保证 ALSA 长期无帧时
    // 仍会取消当前 turn，而不是永久占住会话。
    state_deadline_ = Clock::now() + kMaximumTurnWait;
    ui_.SetState(DeviceUiState::kListening);
    for (std::size_t i = 0U; i < buffered_frames; ++i)
      if (!SendAudioOrRecover(pre_[(pre_head_ + i) % pre_.size()],
                              buffered_utterance_ended && i + 1U == buffered_frames,
                              "pre-roll upload failed")) return false;
    std::cout << "boompi-client: VAD speech started; pre_roll_frames="
              << buffered_frames << std::endl;
    pre_head_ = pre_count_ = 0U;
    if (buffered_utterance_ended) {
      if (!SendControl(ControlKind::kTurnCommit, turn_ids_)) {
        LoseConnection("turn commit failed");
        return false;
      }
      WaitForResponse();
    }
    return true;
  }

  void Commit(const CaptureFrame& frame) {
    if (!SendAudioOrRecover(frame, true, "turn commit failed")) return;
    if (!SendControl(ControlKind::kTurnCommit, turn_ids_)) {
      LoseConnection("turn commit failed");
      return;
    }
    WaitForResponse();
  }

  // 连续近讲一旦形成候选就一次性静音；随后等待硬件参考清零、声学尾音消退和一次完整
  // VAD start。这样打断听感只有一个明确切点，不会经历“降音量 -> 静音”的阶梯卡顿。
  void HandleBarge(const CaptureFrame& frame) {
    if (!config_.barge_in_enabled) {
      ResetBargeProbe();
      return;
    }
    if (barge_probe_ == BargeProbe::kIdle) {
      if (barge_cooldown_frames_ != 0U) {
        --barge_cooldown_frames_; return;
      }
      if (!IsBargeVoice(frame)) {
        barge_probe_frames_ = 0U;
        return;
      }
      if (++barge_probe_frames_ < kBargeCandidateFrames) return;
      // 这 6 帧是已通过 AEC 后近讲检测的句首；进入参考尾音检查时保留它们。
      KeepNewestPreRoll(kBargeCandidateFrames);
      ResetBargeTurn();
      barge_probe_frames_ = 0U;
      barge_probe_ = BargeProbe::kWaitReferenceLow;
      std::cout << "boompi-client: barge-in probe muted; input_dbfs="
                << frame.input_dbfs << "; voice_dbfs=" << frame.voice_dbfs
                << std::endl;
      audio_.SetPlaybackScale(0.0F); return;
    }
    if (barge_probe_ == BargeProbe::kWaitReferenceLow) {
      if (++barge_probe_frames_ > kBargeReferenceWaitFrames) {
        RejectBargeProbe();
        return;
      }
      barge_quiet_frames_ = frame.reference_active
                                ? 0U
                                : barge_quiet_frames_ + 1U;
      if (barge_quiet_frames_ < kBargeReferenceLowFrames) return;
      barge_probe_frames_ = 0U;
      barge_probe_ = BargeProbe::kClear;
      return;
    }
    if (barge_probe_ == BargeProbe::kClear) {
      if (frame.reference_active) {
        RejectBargeProbe();
        return;
      }
      if (++barge_probe_frames_ < kBargeEchoClearFrames) return;
      barge_probe_frames_ = 0U;
      barge_probe_ = BargeProbe::kVerify;
      // 保留候选语音已经建立的 VAD 生命周期。ResetListener 会清掉采集队列中的
      // near_voice，而真实后端还需重新累计 120 ms；kVerify 随即在首个 20 ms
      // 空判定上拒绝，形成“静音 -> 拒绝 -> 重试”的周期性播放断口。
      return;
    }
    if (frame.reference_active || !IsBargeVoice(frame)) {
      RejectBargeProbe();
      return;
    }
    if (++barge_probe_frames_ < kBargeConfirmFrames) return;
    ConfirmBarge(frame);
  }

  void ConfirmBarge(const CaptureFrame& frame) {
    audio_.DropPlayback();
    ResetBargeProbe();
    ClearSubtitle();
    // 当前这句话既是取消证据，也是下一 turn 的输入；不能在这里清掉
    // 已确认状态，否则 cancel ACK 后会错误地要求用户再说第二遍。
    barge_turn_admitted_ = true;
    barge_ended_ = false;
    barge_silence_frames_ = 0U;
    barge_buffer_overflow_ = false;
    finish_after_cancel_ack_ = false;
    std::cout << "boompi-client: barge-in playback cancelled; input_dbfs="
              << frame.input_dbfs << std::endl;
    if (!SendControl(ControlKind::kResponseCancel, response_ids_))
      LoseConnection("response.cancel failed");
    else {
      state_ = State::kCancelPending;
      cancel_started_at_ = Clock::now();
      state_deadline_ = cancel_started_at_ + kCancelAckWait;
    }
  }

  // 唯一的帧驱动入口；网络事件和取消 helper 也可转换状态，且必须共享同一 actor。
  void OnFrame(const CaptureFrame& frame) {
    const bool sequence_gap = frame.sequence != next_capture_sequence_;
    next_capture_sequence_ = frame.sequence + 1U;
    if (frame.discontinuity || sequence_gap) {
      std::cerr << "boompi-client: capture discontinuity; source="
                << (frame.actor_overrun ? "actor_queue" :
                    frame.discontinuity ? "alsa" : "sequence_gap") << std::endl;
      pre_count_ = 0U;
      if (state_ == State::kCancelPending) {
        // cancel 已上行，采集断帧只会让当前打断语音失效；继续等同一 ACK，
        // 不能把本地 ALSA/actor 缺口扩大成持久 WSS 重连。
        ResetBargeTurn();
        finish_after_cancel_ack_ = true;
      }
      else if (state_ == State::kCapturingSpeech ||
               state_ == State::kWaitingForResponse ||
               state_ == State::kPlayingResponse)
        AbortCurrent("capture discontinuity cancel failed");
      else if (state_ == State::kDrainingPlayback)
        AbortCurrent("drain discontinuity cancel failed");
      else {
        audio_.DropPlayback();
        state_ = State::kWaitingForWake;
        ui_.SetState(DeviceUiState::kIdle);
        ResetListener();
      }
      return;
    }
    // 停顿后恢复近讲的首帧必须先解冻，否则会固定丢 20 ms。
    if (state_ == State::kCancelPending && !barge_ended_ && frame.near_voice)
      barge_silence_frames_ = 0U;
    const bool freeze_barge_tail =
        state_ == State::kCancelPending &&
        (barge_ended_ || barge_silence_frames_ >= kBargeTailFreezeFrames);
    if (state_ != State::kCapturingSpeech && !freeze_barge_tail)
      SavePreRoll(frame);
    if (turn_ids_.session == 0U) {
      if (frame.wake) {
        std::cout << "boompi-client: Snowboy wake detected; server offline" << std::endl;
        ResetListener();
      }
      return;
    }
    switch (state_) {
      case State::kWaitingForWake:
        // PollUi 在本帧出队后运行；延后一帧切换可隔离 ResetListener 前
        // 已经生成的旧 VAD 边沿，防止一次触屏直接提交伪 turn。
        if (manual_wake_pending_) {
          manual_wake_pending_ = false;
          BeginListening("touch wake detected");
        } else if (frame.wake) BeginListening("Snowboy wake detected");
        break;
      case State::kWaitingForSpeech:
        if (frame.vad_started) {
          std::cout << "boompi-client: VAD gate opened; input_dbfs="
                    << frame.input_dbfs << "; min_dbfs="
                    << config_.vad_min_dbfs << std::endl;
          StartTurn();
        }
        break;
      case State::kWaitingForFollowUp:
        if (!frame.near_voice) follow_up_near_frames_ = 0U;
        else if (++follow_up_near_frames_ >= kFollowUpNearFrames) {
          std::cout << "boompi-client: follow-up admitted; input_dbfs="
                    << frame.input_dbfs << "; min_dbfs="
                    << config_.vad_min_dbfs << std::endl;
          StartTurn();
        }
        break;
      case State::kCapturingSpeech:
        if (uplink_sequence_ + 1U >= kMaximumTurnFrames) {
          std::cout << "boompi-client: voice input limit reached; committing turn" << std::endl;
          Commit(frame);
        } else if (barge_ended_ || frame.vad_ended) {
          barge_ended_ = false;
          Commit(frame);
        }
        else if (!SendAudioOrRecover(frame, false, "uplink PCM send failed")) return;
        break;
      case State::kPlayingResponse: HandleBarge(frame); break;
      case State::kCancelPending:
        TrackBargeUtterance(frame);
        break;
      case State::kDrainingPlayback: HandleBarge(frame); break;
      default: break;
    }
  }

  // config_ 构造后只读。NetworkLoop 持锁更新 endpoint_，application actor
  // 只在同一锁下拷贝连接快照；其余业务状态只属于 actor。
  const config::VoiceClientConfig config_;
  NetworkBootstrapResult endpoint_;
  std::string* error_;
  AudioEngine audio_;
  ui::DeviceUi ui_;
  std::unique_ptr<VoiceTransport> transport_;
  EventQueue events_;
  std::thread network_thread_;
  std::mutex network_mutex_;
  std::atomic<bool> stop_network_{false};
  std::atomic<bool> endpoint_ready_{false};
  std::atomic<bool> network_needed_{true};
  // 预卷帧与当前 turn 使用同一 capture sequence，便于发现 ALSA xrun 后的伪连续。
  std::array<CaptureFrame, kBargeBufferFrames> pre_{};
  std::size_t pre_head_{0U};
  std::size_t pre_count_{0U};
  WireIds turn_ids_{};
  WireIds response_ids_{};
  std::uint32_t session_epoch_{0U};
  std::uint32_t downlink_sequence_{0U};
  std::string response_id_;
  State state_{State::kWaitingForWake};
  Clock::time_point state_deadline_{};
  Clock::time_point next_connect_{};
  Clock::time_point heartbeat_deadline_{};
  Clock::time_point await_hint_at_{};
  Clock::time_point cancel_started_at_{};
  std::uint64_t next_capture_sequence_{0U};
  std::uint32_t message_{1U};
  std::uint32_t uplink_sequence_{0U};
  std::string subtitle_;
  unsigned reconnect_attempt_{0U};
  unsigned follow_up_near_frames_{0U};
  // 候选计数发生在正常播放音量下；确认计数发生在静音并清掉参考尾音之后。
  // 分开保存可避免读代码时误以为同一累计值跨越了两个声学阶段。
  unsigned barge_probe_frames_{0U};
  unsigned barge_cooldown_frames_{0U};
  unsigned barge_quiet_frames_{0U};
  unsigned barge_silence_frames_{0U};
  std::uint8_t volume_{60U};
  bool await_hint_pending_{false};
  bool await_hint_visible_{false};
  bool response_audio_started_{false};
  bool response_audio_ended_{false};
  BargeProbe barge_probe_{BargeProbe::kIdle};
  bool hello_sent_{false};
  bool barge_ended_{false};
  bool barge_turn_admitted_{false};
  bool barge_buffer_overflow_{false};
  bool manual_wake_pending_{false};
  bool finish_after_cancel_ack_{false};
  bool listener_failed_{false};
};

}  // namespace

bool RunVoiceClient(const config::VoiceClientConfig& config,
                    const volatile std::sig_atomic_t* stop,
                    std::string* const error) {
  try { VoiceClient client(config, error); return client.Run(stop); }
  catch (const std::bad_alloc&) {
    if (error != nullptr) *error = "voice client allocation failed";
  }
  catch (...) {
    if (error != nullptr) *error = "voice client failed unexpectedly";
  }
  return false;
}

}  // namespace boompi::application
