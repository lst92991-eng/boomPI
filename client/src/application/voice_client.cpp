/// @file 对话状态机：只编排已经实现的音频与 WSS 能力，不承载底层算法。
#include "boompi/application/voice_client.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <fstream>
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
using audio::AudioEngine; using audio::AudioEngineConfig; using audio::CaptureFrame;
using network::Control; using network::ControlKind; using network::Inbound; using network::InboundKind;
using network::NetworkBootstrap; using network::NetworkBootstrapResult;
using network::Pcm64Header; using network::SendOutcome; using network::TransportConfig; using network::VoiceTransport; using network::WireIds;
using ui::DeviceUiAction; using ui::DeviceUiState;
constexpr std::size_t kPreRollFrames = 25U;
constexpr unsigned kBargeCandidateFrames = 6U, kBargeReferenceLowFrames = 3U;
constexpr unsigned kBargeReferenceWaitFrames = 15U, kBargeEchoClearFrames = 3U;
constexpr unsigned kBargeConfirmFrames = 3U;
constexpr unsigned kBargeAdmissionQuietFrames = 20U;
constexpr unsigned kBargeTailFreezeFrames = 10U;
constexpr unsigned kBargeRetryCooldownFrames = 15U;
constexpr std::uint32_t kMaximumTurnFrames = 3000U;  // 60 s：AGENTS.md 单次用户语音最长 60 秒。
constexpr auto kFirstResponseHint = std::chrono::seconds(15);  // AGENTS.md：首响等 15 秒给本地提示，30 秒取消。
constexpr auto kBargeAdmissionWait = std::chrono::milliseconds(500);
// Follow-up has no wake word, so it must be stricter than an in-playback barge.
// The 300 ms backend tail guard clears the known render tail first; these
// additional 400 ms must then be continuously classified as near speech.
// The 500 ms pre-roll still preserves the beginning of a genuine utterance.
constexpr unsigned kFollowUpNearFrames = 20U;
constexpr unsigned kEventsPerCaptureFrame = 8U;
constexpr char kUiSettings[] = "/userdata/boompi/config/ui.settings";

/** WSS 线程单生产、主 actor 单消费；固定 64 项，溢出时主动断线而不是覆盖事件。 */
class EventQueue final {
 public:
  bool Push(Inbound event) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (count_ == slots_.size()) { overflow_ = true; return false; }
    slots_[tail_] = std::move(event); tail_ = (tail_ + 1U) % slots_.size(); ++count_; return true;
  }
  bool Pop(Inbound* event) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (count_ == 0U) return false;
    *event = std::move(slots_[head_]); head_ = (head_ + 1U) % slots_.size(); --count_; return true;
  }
  bool overflowed() const { std::lock_guard<std::mutex> lock(mutex_); return overflow_; }
  void Clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    head_ = tail_ = count_ = 0U; overflow_ = false;
  }
 private:
  mutable std::mutex mutex_;
  std::array<Inbound, 64U> slots_{};
  std::size_t head_{0U}, tail_{0U}, count_{0U};
  bool overflow_{false};
};

/// 一轮对话的完整状态：唤醒 -> 等首句 -> 上行 -> 等回复 -> 播放；播放中确认近讲
/// 进入 Barge，播放自然结束进入 Drain，随后保留 3 秒 FollowUp 免唤醒窗口。
enum class State : std::uint8_t {
  kWake, kWaitSpeech, kCapture, kAwait,
  kSpeak, kBarge, kBargeAdmit, kDrain, kFollowUp,
};

/// 播放中近讲探针：一次只产生一个明确切点（直接静音并取消），不走降音量阶梯。
/// 四段状态全部由帧驱动：
///   kIdle  连续 6 帧近讲 -> kMuted（一次性静音），进入 kWaitReferenceLow；
///   kWaitReferenceLow  15 帧内等到连续 3 帧硬件参考安静 -> kReferenceLow，否则 kRejected；
///   kClear  再等 3 帧回声消退（期间参考复活则 kRejected）-> kEchoCleared；
///   kVerify  参考安静且连续 3 帧近讲 -> kConfirmed；任何参考复活/远讲 -> kRejected。
/// kRejected 自带复位与 15 帧冷却；kConfirmed 由调用方复位。本类只持有探针自身
/// 计数，轮次级证据（admit/freeze/tail）在 VoiceClient；两者经 Reset 同步复位。
class BargeProbeMachine final {
 public:
  enum class Event : std::uint8_t { kNone, kMuted, kReferenceLow, kEchoCleared, kRejected, kConfirmed };
  Event OnFrame(const CaptureFrame& frame) {
    switch (stage_) {
      case Stage::kIdle:
        if (cooldown_ != 0U) { --cooldown_; return Event::kNone; }
        if (!frame.near_voice) { near_ = 0U; return Event::kNone; }
        if (++near_ < kBargeCandidateFrames) return Event::kNone;
        near_ = stage_frames_ = 0U; stage_ = Stage::kWaitReferenceLow;
        return Event::kMuted;
      case Stage::kWaitReferenceLow:
        if (++stage_frames_ > kBargeReferenceWaitFrames) return Reject();
        near_ = frame.reference_active ? 0U : near_ + 1U;
        if (near_ < kBargeReferenceLowFrames) return Event::kNone;
        near_ = stage_frames_ = 0U; stage_ = Stage::kClear;
        return Event::kReferenceLow;
      case Stage::kClear:
        if (frame.reference_active) return Reject();
        if (++stage_frames_ < kBargeEchoClearFrames) return Event::kNone;
        near_ = stage_frames_ = 0U; stage_ = Stage::kVerify;
        return Event::kEchoCleared;
      case Stage::kVerify:
        if (frame.reference_active || !frame.near_voice) return Reject();
        if (++near_ < kBargeConfirmFrames) return Event::kNone;
        return Event::kConfirmed;
    }
    return Event::kNone;
  }
  void Reset() {
    stage_ = Stage::kIdle; near_ = stage_frames_ = cooldown_ = 0U;
  }
  // kClear/kVerify 期间轮次级 quiet 证据逐帧累加（拒绝帧除外）。
  bool counting_quiet() const { return stage_ == Stage::kClear || stage_ == Stage::kVerify; }
 private:
  enum class Stage : std::uint8_t { kIdle, kWaitReferenceLow, kClear, kVerify };
  Event Reject() { Reset(); cooldown_ = kBargeRetryCooldownFrames; return Event::kRejected; }
  Stage stage_{Stage::kIdle};
  unsigned near_{0U}, stage_frames_{0U}, cooldown_{0U};
};

/// 会话 actor 导读：主线程即唯一 actor，帧驱动时序为
///   Capture(20ms) -> FinishConnect -> DrainEvents -> PollUi -> OnFrame。
/// WSS 回调线程只能写 EventQueue；audio 的 capture/playback 线程只碰自己的环。
/// 因此以下会话字段无需加锁；状态转换只有三个入口：OnFrame（帧/超时）、
/// HandleEvent（网络事件）和取消 helper（AbortCurrent/CompleteCancel/FinishTurn）。
class VoiceClient final {
 public:
  VoiceClient(const config::VoiceClientConfig& config, std::string* error)
      : config_(config), error_(error) {}
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
    audio_config.capture_pcm = config_.capture_pcm; audio_config.playback_pcm = config_.playback_pcm;
    audio_config.snowboy_resource = config_.snowboy_resource_path;
    audio_config.snowboy_model = config_.snowboy_model_path;
    audio_config.snowboy_sensitivity = config_.snowboy_sensitivity;
    audio_config.vad_min_dbfs = config_.vad_min_dbfs;
    volume_ = config_.volume_percent;
    unsigned saved_volume = volume_, saved_brightness = 100U;
    std::ifstream settings(kUiSettings);
    if (settings >> saved_volume >> saved_brightness && saved_volume <= 100U)
      volume_ = static_cast<std::uint8_t>(saved_volume);
    audio_config.playback_gain = static_cast<float>(volume_) *
                                 config_.speaker_gain_percent / 10000.0F;
    audio_config.left_polarity = config_.capture_left_polarity;
    audio_config.right_polarity = config_.capture_right_polarity;
    if (!ui_.Open()) std::cerr << "boompi-client: display unavailable; continuing voice-only\n";
    brightness_ = saved_brightness != 0U; ui_.SetBrightness(brightness_ ? 100U : 0U);
    ui_.SetState(DeviceUiState::kOffline);
    if (!audio_.Open(audio_config)) return Fail(audio_.last_error());
    try { network_thread_ = std::thread(&VoiceClient::NetworkLoop, this); }
    catch (...) { return Fail("network thread creation failed"); }
    audio_origin_us_ = NowUs(); next_connect_ = Clock::now();
    std::cout << "boompi-client: voice loop ready; wake_word=snowboy"
              << "; wake_sensitivity=" << config_.snowboy_sensitivity
              << "; vad_min_dbfs=" << config_.vad_min_dbfs << std::endl;
    while (stop == nullptr || *stop == 0) {
      if (!transport_ && endpoint_ready_.load(std::memory_order_acquire) &&
          Clock::now() >= next_connect_) StartConnect();
      if (listener_failed_) return Fail(audio_.last_error());
      CaptureFrame frame{};
      if (!audio_.Capture(&frame)) return Fail(audio_.last_error());
      FinishConnect();
      DrainEvents();
      PollUi();
      if (listener_failed_) return Fail(audio_.last_error());
      if (events_.overflowed()) LoseConnection("network event queue overflow");
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
  static std::uint64_t NowUs() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            Clock::now().time_since_epoch()).count());
  }
  bool SendControl(ControlKind kind, WireIds ids) {
    Control control{}; control.kind = kind;
    control.message_id = "client-" + std::to_string(message_++); control.ids = ids;
    if (kind == ControlKind::kHello) control.device_token = config_.device_token;
    return transport_ && transport_->SendControl(control);
  }
  void ResetListener() { if (!audio_.ResetListener()) listener_failed_ = true; }
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
  void SaveUiSettings() {
    constexpr char temporary[] = "/userdata/boompi/config/ui.settings.tmp";
    std::ofstream output(temporary, std::ios::trunc);
    output << static_cast<unsigned>(volume_) << ' ' << (brightness_ ? 100 : 0) << '\n';
    output.close(); if (output) std::rename(temporary, kUiSettings);
  }
  void PollUi() {
    DeviceUiAction action{}; if (!ui_.PollAction(&action)) return;
    if (action == DeviceUiAction::kWake) {
      if (state_ == State::kWake && ids_.session != 0U)
        manual_wake_pending_ = true;
      else if (ids_.session == 0U)
        std::cout << "boompi-client: touch wake ignored; server offline" << std::endl;
      return;
    }
    if (action == DeviceUiAction::kInterrupt &&
        (state_ == State::kSpeak || state_ == State::kDrain)) {
      subtitle_.clear(); ui_.SetText({}, {});
      if (state_ == State::kDrain) { audio_.DropPlayback(); FinishTurn(false); }
      else AbortCurrent("touch interrupt cancel failed");
      return;
    }
    if (action == DeviceUiAction::kVolumeUp || action == DeviceUiAction::kVolumeDown) {
      const int step = action == DeviceUiAction::kVolumeUp ? 5 : -5;
      volume_ = static_cast<std::uint8_t>(std::clamp(static_cast<int>(volume_) + step, 0, 100));
      audio_.SetPlaybackGain(static_cast<float>(volume_) * config_.speaker_gain_percent / 10000.0F);
      SaveUiSettings();
    } else if (action == DeviceUiAction::kBrightnessUp) {
      brightness_ = true; ui_.SetBrightness(100U); SaveUiSettings();
    } else if (action == DeviceUiAction::kBrightnessDown) {
      brightness_ = false; ui_.SetBrightness(0U); SaveUiSettings();
    }
  }
  // 探针自身复位：计数归零并恢复音量；near_frames_ 同时供 kFollowUp 复用，必须一起清。
  void ResetBargeProbe() {
    probe_.Reset(); near_frames_ = 0U; audio_.SetPlaybackScale(1.0F);
  }
  // 轮次级 barge 证据的统一复位：探针拒绝、取消、断线、回合结束共用，避免多处漂移。
  void ResetBargeState() {
    barge_ended_ = barge_turn_admitted_ = barge_vad_started_seen_ = false;
    barge_quiet_frames_ = barge_silence_frames_ = 0U; barge_tail_frozen_ = false;
  }
  // 探针副作用（静音、复位监听器、拒绝日志）留在 actor；阶段计数全在 BargeProbeMachine。
  // 逐帧语义与提取前一致：拒绝帧不累加 quiet，转换帧按原顺序执行副作用。
  // 唯一有意差异：拒绝时顺带清除可能残留的上一轮 admit/ended 证据，比旧实现更严格。
  void HandleBarge(const CaptureFrame& frame, bool response_active) {
    if (!config_.barge_in_enabled) { ResetBargeProbe(); return; }
    using ProbeEvent = BargeProbeMachine::Event;
    const ProbeEvent event = probe_.OnFrame(frame);
    if (event == ProbeEvent::kRejected) {
      std::cout << "boompi-client: barge-in probe rejected" << std::endl;
      audio_.SetPlaybackScale(1.0F); ResetBargeState(); return;
    }
    if (event == ProbeEvent::kMuted) {
      barge_quiet_frames_ = 0U; barge_vad_started_seen_ = false;
      barge_silence_frames_ = 0U; barge_tail_frozen_ = false;
      std::cout << "boompi-client: barge-in probe muted; input_dbfs="
                << frame.input_dbfs << std::endl;
      audio_.SetPlaybackScale(0.0F); return;
    }
    if (event == ProbeEvent::kReferenceLow) {
      // Drop playback-era history, but preserve the three newest frames whose
      // hardware reference was already quiet so a real short phrase keeps its lead.
      KeepNewestPreRoll(kBargeReferenceLowFrames);
      barge_quiet_frames_ = kBargeReferenceLowFrames; return;
    }
    if (probe_.counting_quiet() && barge_quiet_frames_ < kBargeAdmissionQuietFrames)
      ++barge_quiet_frames_;
    if (event == ProbeEvent::kEchoCleared) { ResetListener(); return; }
    if (event == ProbeEvent::kConfirmed) ConfirmBarge(response_active, frame);
  }

  void BeginListening(const char* reason) {
    state_ = State::kWaitSpeech;
    deadline_ = Clock::now() + std::chrono::seconds(6);
    ui_.SetState(DeviceUiState::kListening);
    std::cout << "boompi-client: " << reason << std::endl;
    ResetListener();
  }

  void NetworkLoop() {
    const bool discover = config_.server_ip.empty();
    while (!stop_network_.load(std::memory_order_acquire)) {
      if (!network_needed_.load(std::memory_order_acquire)) { std::this_thread::sleep_for(std::chrono::milliseconds(100)); continue; }
      NetworkBootstrapResult found{};
      // 传入 stop 观察点：退出时 DHCP/配网等待不再阻塞最长 12 秒。
      if (NetworkBootstrap::Start(discover, &found, &stop_network_)) {
        std::lock_guard<std::mutex> lock(network_mutex_);
        config_.server_ip = std::move(found.host); config_.server_port = found.port;
        config_.server_spki_sha256 = std::move(found.spki_sha256_base64);
        endpoint_ready_.store(true, std::memory_order_release); network_needed_.store(false, std::memory_order_release);
      }
      for (unsigned i = 0U; i != 20U && !stop_network_.load(std::memory_order_acquire); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  }

  // Connect 只启动 WSS/ASIO 线程；hello 必须等 open callback 确认 connected 后由 actor 发送。
  void StartConnect() {
    try {
      transport_ = std::make_unique<VoiceTransport>(); hello_sent_ = false;
      deadline_ = Clock::now() + std::chrono::seconds(6);
      TransportConfig tc{}; std::lock_guard<std::mutex> lock(network_mutex_);
      tc.host = config_.server_ip; tc.port = config_.server_port;
      tc.device_id = config_.device_id; tc.spki_sha256_base64 = config_.server_spki_sha256;
      if (!transport_->Connect(std::move(tc), [this](Inbound e) { events_.Push(std::move(e)); },
          [this](std::string why) {
            Inbound e{}; e.kind = InboundKind::kError;
            e.error_code = "transport"; e.text = std::move(why);
            events_.Push(std::move(e));
          }))
        LoseConnection("connection startup failed");
    } catch (...) { LoseConnection("connection startup failed"); }
  }

  void FinishConnect() {
    if (!transport_ || hello_sent_ || !transport_->connected()) return;
    if (!SendControl(ControlKind::kHello, {})) LoseConnection("hello send failed");
    else { hello_sent_ = true; deadline_ = Clock::now() + std::chrono::seconds(5); }
  }

  // 连接错误会使当前 turn 失效。旧事件、旧音频和旧 epoch 必须一起清掉，不能重传。
  void LoseConnection(const char* reason) {
    if (transport_) transport_->Close();
    transport_.reset(); events_.Clear(); audio_.DropPlayback(); audio_.SetPlaybackScale(1.0F);
    ui_.SetState(DeviceUiState::kOffline);
    state_ = State::kWake; ids_ = response_ = {};
    pre_head_ = pre_count_ = 0U; ResetBargeProbe(); ResetBargeState();
    finish_after_start_ = finish_after_cancel_ = false;
    ResetListener();
    const unsigned seconds = reconnect_attempt_ < 5U ? 1U << reconnect_attempt_ : 30U;
    if (reconnect_attempt_ < 5U) ++reconnect_attempt_;
    next_connect_ = Clock::now() + std::chrono::seconds(seconds); network_needed_.store(true, std::memory_order_release);
    std::cerr << "boompi-client: network retry in " << seconds << "s; reason=" << reason << std::endl;
  }

  void DrainEvents() {
    Inbound event{};
    // capture 环只有 4×20 ms（80 ms）缓冲；限制单帧事件消费量，避免持续下行饿死采集 actor。
    for (unsigned handled = 0U;
         handled < kEventsPerCaptureFrame && transport_ && !listener_failed_ &&
         events_.Pop(&event);
         ++handled) {
      HandleEvent(std::move(event));
    }
  }

  // 本地音频连续性/容量故障只取消当前轮次；WSS/TLS 仍健康时不能丢掉持久 provider 会话。
  void AbortCurrent(const char* failure) {
    audio_.DropPlayback(); ResetBargeProbe(); finish_after_cancel_ = true;
    const bool has_response = response_.session != 0U;
    if (!SendControl(has_response ? ControlKind::kResponseCancel : ControlKind::kTurnCancel,
                     has_response ? response_ : ids_)) {
      LoseConnection(failure); return;
    }
    state_ = State::kBarge; deadline_ = Clock::now() + std::chrono::seconds(3);
  }

  // WSS 回调只生产 Inbound；所有协议事件都在 actor 线程内改变 conversation state。
  void HandleEvent(Inbound event) {
    if (event.kind == InboundKind::kError) {
      std::cerr << "boompi-client: " << event.error_code << ": " << event.text << std::endl;
      if (event.error_code == "transport" || ids_.session == 0U) LoseConnection("transport or hello error");
      else if (event.ids.turn == 0U) LoseConnection("session error");
      else if (event.error_code == "downlink_discontinuity") {
        if (state_ != State::kBarge) AbortCurrent("discontinuous response cancel failed");
      }
      else if (state_ == State::kBarge) CompleteCancel();
      else FinishTurn(false);
      return;
    }
    // 服务端 writePump 无条件每 ≤10s 发 ping（配置校验强制上限），30s 判活留足余量。
    if (event.kind == InboundKind::kHeartbeat) { heartbeat_deadline_ = Clock::now() + std::chrono::seconds(30); return; }
    if (state_ == State::kBarge && event.kind == InboundKind::kDone) { CompleteCancel(); return; }
    if (state_ == State::kBarge && event.kind != InboundKind::kCancelled) return;
    // The response has already been cancelled in kBargeAdmit.  Stale response
    // events must not extend its bounded acoustic-admission window.
    if (state_ == State::kBargeAdmit) return;
    deadline_ = Clock::now() + std::chrono::seconds(30);
    switch (event.kind) {
      case InboundKind::kHelloAck:
        ids_ = {event.ids.session, 1U, 1U, event.ids.epoch}; reconnect_attempt_ = 0U; heartbeat_deadline_ = deadline_;
        pre_head_ = pre_count_ = 0U; ResetListener();
        ui_.SetState(DeviceUiState::kIdle);
        std::cout << "boompi-client: secure session ready" << std::endl; break;
      case InboundKind::kResponseStart:
        response_ = event.ids; subtitle_.clear(); ui_.SetText({}, {});
        break;
      case InboundKind::kTextDelta: ShowText(event.text); break;
      case InboundKind::kAudioStart:
        if (!audio_.BeginPlayback()) { LoseConnection("playback start failed"); break; }
        state_ = State::kSpeak; ui_.SetState(DeviceUiState::kSpeaking); ResetBargeProbe(); break;
      case InboundKind::kPcm:
        if (!audio_.QueueTts24k(event.audio.data(), event.audio_size, event.pcm.sequence))
          AbortCurrent("TTS response cancel failed");
        break;
      case InboundKind::kDone:
        if (audio_.stream_open() && !audio_.EndPlayback()) LoseConnection("playback finish failed");
        // 尾播仍出声，UI 用 kSpeakingTail 保留触屏打断入口；
        // kHappy 留给 FinishTurn 后的追问窗口（不可打断）。
        else { state_ = State::kDrain; ui_.SetState(DeviceUiState::kSpeakingTail); }
        break;
      case InboundKind::kCancelled:
        if (state_ == State::kBarge) CompleteCancel();
        else FinishTurn(false);
        break;
      default: break;
    }
  }

  void AdvanceTurn() {
    ++ids_.turn; ++ids_.stream; ++ids_.epoch; response_ = {};
  }

  // 正常结束和异常取消都推进 epoch 并进入 follow-up；异常路径还会立即丢弃残余播放。
  void FinishTurn(bool normal) {
    ResetBargeProbe(); ResetBargeState();
    finish_after_start_ = finish_after_cancel_ = false;
    if (!normal) audio_.DropPlayback();
    AdvanceTurn();
    ResetListener();
    state_ = State::kFollowUp; deadline_ = Clock::now() + std::chrono::seconds(3);
    ui_.SetState(DeviceUiState::kHappy);
  }

  void ResumeAfterBarge() {
    // Canceling playback and admitting a new user turn are separate decisions.
    // Never upload muted playback residue merely because cancel ACK arrived.
    if (!barge_turn_admitted_) { FinishTurn(false); return; }
    barge_turn_admitted_ = false;
    audio_.DropPlayback(); audio_.SetPlaybackScale(1.0F); finish_after_start_ = barge_ended_; finish_after_cancel_ = false;
    AdvanceTurn(); StartTurn();
  }

  void CompleteCancel() {
    if (finish_after_cancel_) { FinishTurn(false); return; }
    // Network events are drained before the current capture frame is examined.
    // Defer every resume decision to kBargeAdmit so that frame can still revoke
    // stale evidence or report a discontinuity before a new turn starts.
    state_ = State::kBargeAdmit;
    deadline_ = Clock::now() + kBargeAdmissionWait;
    ui_.SetState(DeviceUiState::kListening);
  }

  // pre-roll 是 25 个固定槽的滚动窗；保存的是 AEC 后 PCM，发送时仍校验 capture sequence。
  void SavePreRoll(const CaptureFrame& frame) {
    pre_[(pre_head_ + pre_count_) % pre_.size()] = frame;
    if (pre_count_ < pre_.size()) ++pre_count_; else pre_head_ = (pre_head_ + 1U) % pre_.size();
  }

  void KeepNewestPreRoll(const std::size_t count) {
    const std::size_t keep = std::min(count, pre_count_);
    pre_head_ = (pre_head_ + pre_count_ - keep) % pre_.size();
    pre_count_ = keep;
  }

  bool IsValidBargeStart(const CaptureFrame& frame) const {
    return frame.vad_started && frame.near_voice && !frame.reference_active &&
        frame.input_dbfs >= config_.vad_min_dbfs;
  }

  void PrepareBargePreRoll(const CaptureFrame& frame) {
    if (!barge_tail_frozen_ || !frame.near_voice || frame.reference_active) return;
    if (barge_ended_) {
      // A late cancel ACK can overlap a second utterance.  Only a fresh VAD
      // start may replace the frozen first utterance; a single noisy frame may not.
      if (!IsValidBargeStart(frame)) return;
      barge_ended_ = false; barge_vad_started_seen_ = true;
      pre_head_ = pre_count_ = 0U;
    }
    barge_silence_frames_ = 0U; barge_tail_frozen_ = false;
  }

  void ObserveBargeAdmission(const CaptureFrame& frame) {
    if (frame.reference_active) {
      barge_quiet_frames_ = 0U; barge_vad_started_seen_ = false;
      barge_turn_admitted_ = false;
    } else if (barge_quiet_frames_ < kBargeAdmissionQuietFrames) {
      ++barge_quiet_frames_;
    }
    if (IsValidBargeStart(frame))
      barge_vad_started_seen_ = true;
    // Digital reference silence precedes the acoustic speaker tail.  Require
    // 400 ms of continuous reference-low time as well as a fresh VAD start
    // before the cancelled response may become a new uploaded user turn.
    if (!barge_turn_admitted_ && barge_vad_started_seen_ &&
        barge_quiet_frames_ >= kBargeAdmissionQuietFrames &&
        frame.near_voice && frame.input_dbfs >= config_.vad_min_dbfs) {
      barge_turn_admitted_ = true;
      std::cout << "boompi-client: barge-in turn admitted; input_dbfs="
                << frame.input_dbfs << std::endl;
    }
    // Preserve a short natural pause, then freeze before the backend's 700 ms
    // end hangover can overwrite all speech in the 500 ms pre-roll ring.
    if (barge_turn_admitted_ && frame.near_voice) {
      barge_silence_frames_ = 0U; barge_tail_frozen_ = false;
    } else if (barge_turn_admitted_ &&
               barge_silence_frames_ < kBargeTailFreezeFrames &&
               ++barge_silence_frames_ >= kBargeTailFreezeFrames) {
      barge_tail_frozen_ = true;
    }
    if (frame.vad_ended) { barge_ended_ = true; barge_tail_frozen_ = true; }
  }

  SendOutcome SendAudio(const CaptureFrame& frame, bool end) {
    Pcm64Header h{};
    h.flags = static_cast<std::uint16_t>(
        (uplink_sequence_ == 0U ? 1U : 0U) | (end ? 2U : 0U));
    h.sequence = uplink_sequence_; h.timestamp_us = audio_origin_us_ + frame.sequence * 20000U; h.ids = ids_;
    const SendOutcome outcome = transport_->SendPcm64(
        h, reinterpret_cast<const std::uint8_t*>(frame.pcm.data()),
        frame.pcm.size() * sizeof(frame.pcm[0]));
    if (outcome == SendOutcome::kOk) { ++uplink_sequence_; ++turn_frames_; }
    return outcome;
  }

  // 上行失败按语义分流：瞬时背压只作废当前轮（云端 sequence 将出现缺口，
  // 继续发帧必被拒），连接/协议错误才重建会话。极端背压下取消帧自身也可能
  // 被缓冲拒绝，此时退化为重建连接——有界且可恢复。
  bool SendAudioOrRecover(const CaptureFrame& frame, bool end, const char* rejected_reason) {
    const SendOutcome outcome = SendAudio(frame, end);
    if (outcome == SendOutcome::kOk) return true;
    if (outcome == SendOutcome::kBackpressure) { std::cout << "boompi-client: uplink backpressure; aborting turn" << std::endl; AbortCurrent("uplink backpressure cancel failed"); }
    else LoseConnection(rejected_reason);
    return false;
  }

  // turn.start 成功后先发送最老的 pre-roll，再发送实时帧，确保唤醒/打断不吃掉句首。
  bool StartTurn() {
    if (ids_.session == 0U ||
        !SendControl(ControlKind::kTurnStart, ids_)) {
      LoseConnection("turn.start failed"); return false;
    }
    uplink_sequence_ = turn_frames_ = 0U; state_ = State::kCapture;
    ui_.SetState(DeviceUiState::kListening);
    for (std::size_t i = 0U; i < pre_count_; ++i)
      if (!SendAudioOrRecover(pre_[(pre_head_ + i) % pre_.size()], false,
                              "pre-roll upload failed")) return false;
    std::cout << "boompi-client: VAD speech started; pre_roll_frames=" << pre_count_ << std::endl;
    pre_head_ = pre_count_ = 0U;
    return true;
  }

  void Commit(const CaptureFrame& frame) {
    if (!SendAudioOrRecover(frame, true, "turn commit failed")) return;
    if (!SendControl(ControlKind::kTurnCommit, ids_)) { LoseConnection("turn commit failed"); return; }
    state_ = State::kAwait; deadline_ = Clock::now() + std::chrono::seconds(30);
    await_hint_at_ = Clock::now() + kFirstResponseHint; await_hint_shown_ = false;
    ui_.SetState(DeviceUiState::kThinking);
    std::cout << "boompi-client: VAD speech ended; voice turn committed" << std::endl;
  }

  void ConfirmBarge(bool response_active, const CaptureFrame& frame) {
    audio_.DropPlayback(); ResetBargeProbe();
    subtitle_.clear(); ui_.SetText({}, {});
    // 准入证据（连续参考安静时长与新 VAD start）必须在 kBarge 内重新积累：
    // 探针期残留的旧值会把承诺的 400 ms 压缩到不足十帧。
    ResetBargeState(); finish_after_cancel_ = false;
    std::cout << "boompi-client: barge-in playback cancelled; input_dbfs="
              << frame.input_dbfs << std::endl;
    if (!response_active) { CompleteCancel(); return; }
    if (!SendControl(ControlKind::kResponseCancel, response_)) LoseConnection("response.cancel failed");
    else {
      state_ = State::kBarge;
      deadline_ = Clock::now() + std::chrono::seconds(3);
    }
  }

  // capture 断帧（ALSA xrun 或 actor 队列满）：pre-roll 不再可信；正在取消的轮次
  // 无法保证云端收到 cancel，只能重建会话；其余状态降级为取消当前轮或回到唤醒。
  void HandleDiscontinuity(const CaptureFrame& frame) {
    std::cerr << "boompi-client: capture discontinuity; source="
              << (frame.actor_overrun ? "actor_queue" : "alsa") << std::endl;
    pre_count_ = 0U;
    if (state_ == State::kBarge || state_ == State::kBargeAdmit)
      LoseConnection("capture discontinuity while cancelling");
    else if (state_ == State::kCapture || state_ == State::kAwait ||
             state_ == State::kSpeak)
      AbortCurrent("capture discontinuity cancel failed");
    // kDrain 已消费 response.done，云端没有可取消对象，也不会再发送 cancelled。
    else if (state_ == State::kDrain) FinishTurn(false);
    else { audio_.DropPlayback(); state_ = State::kWake;
           ui_.SetState(DeviceUiState::kIdle); ResetListener(); }
  }

  // 所有有界等待的超时出口集中在此：等话/追问回空闲，取消/排空/承认回回合收尾，
  // 上行与首响等待走 AbortCurrent。返回 true 表示本帧已被超时消费。
  bool ExpireDeadlines(const Clock::time_point& now) {
    if (ids_.session == 0U) {
      if (transport_ && now >= deadline_) LoseConnection("connect or hello timeout");
      return false;  // 无会话时帧仍要处理唤醒词，不算超时消费。
    }
    if (now >= heartbeat_deadline_) { LoseConnection("server heartbeat timed out"); return true; }
    const bool timed_state = state_ == State::kWaitSpeech ||
        state_ == State::kAwait || state_ == State::kSpeak ||
        state_ == State::kBarge || state_ == State::kBargeAdmit ||
        state_ == State::kDrain ||
        state_ == State::kFollowUp;
    if (!timed_state || now < deadline_) return false;
    if (state_ == State::kWaitSpeech || state_ == State::kFollowUp) {
      state_ = State::kWake; pre_head_ = pre_count_ = 0U;
      ui_.SetState(DeviceUiState::kIdle);
      ResetListener(); return true;
    }
    if (state_ == State::kBarge) LoseConnection("voice cancel timed out");
    else if (state_ == State::kBargeAdmit) FinishTurn(false);
    else if (state_ == State::kDrain) FinishTurn(false);
    else AbortCurrent("voice timeout cancel failed");
    return true;
  }

  // 唯一的帧驱动入口；网络事件和取消 helper 也可转换状态，且必须共享同一 actor。
  void OnFrame(const CaptureFrame& frame) {
    if (frame.discontinuity) { HandleDiscontinuity(frame); return; }
    if (state_ == State::kBarge || state_ == State::kBargeAdmit)
      PrepareBargePreRoll(frame);
    const bool freeze_barge_tail =
        (state_ == State::kBarge || state_ == State::kBargeAdmit) && barge_tail_frozen_;
    if (state_ != State::kCapture && !freeze_barge_tail) SavePreRoll(frame);
    const auto now = Clock::now();
    if (ids_.session == 0U) {
      if (frame.wake) {
        std::cout << "boompi-client: Snowboy wake detected; server offline" << std::endl; ResetListener();
      }
      ExpireDeadlines(now); return;
    }
    if (ExpireDeadlines(now)) return;
    // 首响等待超过 15 秒时给一次本地状态提示（AGENTS.md）；不改变 30 秒取消线。
    if (state_ == State::kAwait && !await_hint_shown_ && now >= await_hint_at_) {
      await_hint_shown_ = true; ui_.SetText("还在思考", "云端正在组织回答，请稍候");
      std::cout << "boompi-client: first response pending 15s; showing local hint" << std::endl;
    }
    DispatchFrame(frame);
  }

  // 每个状态只回答一个问题：唤醒后等首句、首句后上行、上行后等回复、回复后播放、
  // 播放中只找打断切点、排空后给 3 秒免唤醒追问。
  void DispatchFrame(const CaptureFrame& frame) {
    switch (state_) {
      case State::kWake:
        if (frame.wake || std::exchange(manual_wake_pending_, false)) {
          BeginListening(frame.wake ? "Snowboy wake detected" : "touch wake detected");
        }
        break;
      case State::kWaitSpeech:
        if (frame.vad_started) {
          std::cout << "boompi-client: VAD gate opened; input_dbfs="
                    << frame.input_dbfs << "; min_dbfs="
                    << config_.vad_min_dbfs << std::endl;
          StartTurn();
        }
        break;
      case State::kFollowUp:
        if (!frame.near_voice) near_frames_ = 0U;
        else if (++near_frames_ >= kFollowUpNearFrames) {
          std::cout << "boompi-client: follow-up admitted; input_dbfs="
                    << frame.input_dbfs << "; min_dbfs="
                    << config_.vad_min_dbfs << std::endl;
          StartTurn();
        }
        break;
      case State::kCapture:
        if (turn_frames_ >= kMaximumTurnFrames) {
          std::cout << "boompi-client: voice input limit reached; cancelling turn" << std::endl;
          AbortCurrent("voice input limit cancel failed");
        }
        else if (finish_after_start_ || frame.vad_ended) {
          finish_after_start_ = false; Commit(frame);
        }
        else if (!SendAudioOrRecover(frame, false, "uplink PCM send failed")) return;
        break;
      case State::kSpeak: HandleBarge(frame, true); break;
      case State::kBarge: ObserveBargeAdmission(frame); break;
      case State::kBargeAdmit:
        ObserveBargeAdmission(frame);
        if (barge_turn_admitted_) ResumeAfterBarge();
        else if (barge_ended_) FinishTurn(false);
        break;
      case State::kDrain:
        if (audio_.playback_done()) {
          if (audio_.playback_failed()) { listener_failed_ = true; return; }
          ResetBargeProbe();
          FinishTurn(true);
        } else HandleBarge(frame, false);
        break;
      default: break;
    }
  }

  // 以下业务状态只由 Run 所在线程读写；WSS 线程只能碰 EventQueue。
  config::VoiceClientConfig config_;
  std::string* error_;
  AudioEngine audio_;
  ui::DeviceUi ui_;
  std::unique_ptr<VoiceTransport> transport_;
  EventQueue events_;
  std::thread network_thread_; std::mutex network_mutex_;
  std::atomic<bool> stop_network_{false}, endpoint_ready_{false}, network_needed_{true};
  // 预卷帧与当前 turn 使用同一 capture sequence，便于发现 ALSA xrun 后的伪连续。
  std::array<CaptureFrame, kPreRollFrames> pre_{};
  std::size_t pre_head_{0U}, pre_count_{0U};
  WireIds ids_{}, response_{};
  State state_{State::kWake};
  Clock::time_point deadline_{}, next_connect_{}, heartbeat_deadline_{}, await_hint_at_{};
  bool await_hint_shown_{false}, manual_wake_pending_{false};
  std::uint64_t audio_origin_us_{0U};
  std::uint32_t message_{1U}, uplink_sequence_{0U}, turn_frames_{0U};
  std::string subtitle_;
  unsigned reconnect_attempt_{0U}, near_frames_{0U};
  unsigned barge_quiet_frames_{0U}, barge_silence_frames_{0U};
  std::uint8_t volume_{60U};
  bool brightness_{true};
  BargeProbeMachine probe_;
  bool hello_sent_{false}, barge_ended_{false}, barge_turn_admitted_{false};
  bool barge_vad_started_seen_{false}, barge_tail_frozen_{false};
  bool finish_after_start_{false}, finish_after_cancel_{false};
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
