/// @file 对话状态机：只编排已经实现的音频与 WSS 能力，不承载底层算法。
#include "boompi/application/voice_client.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <utility>

#include "boompi/audio/audio_engine.h"
#include "boompi/network/voice_transport.h"

namespace boompi::application {
namespace {

using Clock = std::chrono::steady_clock;
using audio::AudioEngine; using audio::AudioEngineConfig; using audio::CaptureFrame;
using network::Control; using network::ControlKind; using network::Inbound; using network::InboundKind;
using network::Pcm64Header; using network::TransportConfig; using network::VoiceTransport; using network::WireIds;
constexpr std::size_t kPreRollFrames = 25U;
constexpr unsigned kBargeReferenceLowFrames = 3U;
constexpr unsigned kBargeReferenceWaitFrames = 15U;
// 200 ms covers the measured render queue, DSP frame, acoustic path and frame
// phase after muting. This is a containment window, not an AEC delay estimate.
constexpr unsigned kBargeEchoClearFrames = 10U;
constexpr unsigned kBargeConfirmFrames = 6U;
// Follow-up has no wake word, so it must be stricter than an in-playback barge.
// The 300 ms backend tail guard clears the known render tail first; these
// additional 400 ms must then be continuously classified as near speech.
// The 500 ms pre-roll still preserves the beginning of a genuine utterance.
constexpr unsigned kFollowUpNearFrames = 20U;
constexpr unsigned kEventsPerCaptureFrame = 8U;

bool SameTurn(const WireIds& left, const WireIds& right) {
  return left.session != 0U && left.session == right.session &&
      left.turn == right.turn && left.epoch == right.epoch;
}

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
  kSpeak, kBarge, kDrain, kFollowUp,
};
enum class BargeProbe : std::uint8_t { kIdle, kWaitReferenceLow, kClear, kVerify };

class VoiceClient final {
 public:
  explicit VoiceClient(const config::VoiceClientConfig& config) : config_(config) {}
  ~VoiceClient() {
    if (transport_) transport_->Close();
    audio_.Close();
  }

  Status Run(const volatile std::sig_atomic_t* stop) {
    // 主线程就是 application actor。Capture 每 20 ms 返回一次，因此连接完成、网络事件
    // 和状态 deadline 都在同一串行时序中处理，不需要给会话字段再加锁。
    AudioEngineConfig audio_config{};
    audio_config.capture_pcm = config_.capture_pcm; audio_config.playback_pcm = config_.playback_pcm;
    audio_config.snowboy_resource = config_.snowboy_resource_path;
    audio_config.snowboy_model = config_.snowboy_model_path;
    audio_config.snowboy_sensitivity = config_.snowboy_sensitivity;
    audio_config.playback_gain = static_cast<float>(config_.volume_percent) *
                                 config_.speaker_gain_percent / 10000.0F;
    audio_config.left_polarity = config_.capture_left_polarity;
    audio_config.right_polarity = config_.capture_right_polarity;
    if (!audio_.Open(audio_config)) return Fail(audio_.last_error());
    audio_origin_us_ = NowUs(); next_connect_ = Clock::now();
    std::cout << "boompi-client: voice loop ready; wake_word=snowboy" << std::endl;
    while (stop == nullptr || *stop == 0) {
      if (!transport_ && Clock::now() >= next_connect_) StartConnect();
      if (listener_failed_) return Fail(audio_.last_error());
      CaptureFrame frame{};
      if (!audio_.Capture(&frame)) return Fail(audio_.last_error());
      FinishConnect();
      DrainEvents();
      if (listener_failed_) return Fail(audio_.last_error());
      if (events_.overflowed()) LoseConnection("network event queue overflow");
      OnFrame(frame);
      if (listener_failed_) return Fail(audio_.last_error());
    }
    return Status::Ok();
  }

 private:
  static Status Fail(std::string why) { return Status::Error(StatusCode::kInternal, std::move(why)); }
  static std::uint64_t NowUs() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            Clock::now().time_since_epoch()).count());
  }
  std::string MessageId() { return "client-" + std::to_string(message_++); }
  bool SendControl(ControlKind kind, WireIds ids) {
    Control control{}; control.kind = kind; control.message_id = MessageId(); control.ids = ids;
    if (kind == ControlKind::kHello) control.device_token = config_.device_token;
    return transport_ && transport_->SendControl(control);
  }
  void ResetListener() { if (!audio_.ResetListener()) listener_failed_ = true; }
  void ResetBargeProbe() {
    near_frames_ = probe_frames_ = 0U; barge_probe_ = BargeProbe::kIdle; audio_.Duck(false);
  }

  // Connect 只启动 WSS/ASIO 线程；hello 必须等 open callback 确认 connected 后由 actor 发送。
  void StartConnect() {
    try {
      transport_ = std::make_unique<VoiceTransport>(); hello_sent_ = false;
      deadline_ = Clock::now() + std::chrono::seconds(6);
      TransportConfig tc{}; tc.host = config_.server_ip; tc.port = config_.server_port;
      tc.device_id = config_.device_id; tc.spki_sha256_base64 = config_.server_spki_sha256;
      if (!transport_->Connect(std::move(tc), [this](Inbound e) { events_.Push(std::move(e)); },
          [this](std::string why) {
            Inbound e{}; e.kind = InboundKind::kError;
            e.error_code = "transport"; e.text = std::move(why);
            events_.Push(std::move(e));
          }, [this](std::uint16_t, std::string why) {
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
    transport_.reset(); events_.Clear(); audio_.DropPlayback(); audio_.Duck(false);
    state_ = State::kWake; ids_ = response_ = {};
    pre_head_ = pre_count_ = 0U; ResetBargeProbe();
    barge_ended_ = finish_after_start_ = finish_after_cancel_ = false;
    ResetListener();
    const unsigned seconds = reconnect_attempt_ < 5U ? 1U << reconnect_attempt_ : 30U;
    if (reconnect_attempt_ < 5U) ++reconnect_attempt_;
    next_connect_ = Clock::now() + std::chrono::seconds(seconds);
    std::cerr << "boompi-client: network retry in " << seconds << "s; reason=" << reason << std::endl;
  }

  void DrainEvents() {
    Inbound event{};
    // capture PCM 只有 40 ms 缓冲；限制单帧事件消费量，避免持续下行饿死采集 actor。
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
      if (event.error_code != "transport" && ids_.session != 0U) {
        const bool session_error = event.ids.session == ids_.session &&
            event.ids.turn == 0U && event.ids.stream == 0U;
        if (!session_error && !SameTurn(event.ids, ids_)) return;
      }
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
    if (state_ == State::kBarge && event.kind == InboundKind::kDone) { CompleteCancel(); return; }
    if (state_ == State::kBarge && event.kind != InboundKind::kCancelled) return;
    deadline_ = Clock::now() + std::chrono::seconds(30);
    switch (event.kind) {
      case InboundKind::kHelloAck:
        ids_ = {event.ids.session, 1U, 1U, event.ids.epoch}; reconnect_attempt_ = 0U;
        pre_head_ = pre_count_ = 0U; ResetListener();
        std::cout << "boompi-client: secure session ready" << std::endl; break;
      case InboundKind::kResponseStart:
        response_ = event.ids; std::cout << "boompi-client: AI: " << std::flush; break;
      case InboundKind::kTextDelta: std::cout << event.text << std::flush; break;
      case InboundKind::kAudioStart:
        if (!audio_.BeginPlayback()) { LoseConnection("playback start failed"); break; }
        state_ = State::kSpeak; ResetBargeProbe(); break;
      case InboundKind::kPcm:
        if (!audio_.QueueTts24k(event.audio.data(), event.audio_size, event.pcm.sequence))
          AbortCurrent("TTS response cancel failed");
        break;
      case InboundKind::kDone:
        std::cout << std::endl;
        if (audio_.playing() && !audio_.EndPlayback()) LoseConnection("playback finish failed");
        else state_ = State::kDrain;
        break;
      case InboundKind::kCancelled:
        if (state_ == State::kBarge) CompleteCancel();
        else FinishTurn(false);
        break;
      default: break;
    }
  }

  bool AdvanceTurn() {
    if (ids_.turn == UINT32_MAX || ids_.stream == UINT32_MAX || ids_.epoch == UINT32_MAX) {
      LoseConnection("voice identifiers exhausted"); return false;
    }
    ++ids_.turn; ++ids_.stream; ++ids_.epoch; response_ = {}; return true;
  }

  // 正常结束和异常取消都推进 epoch 并进入 follow-up；异常路径还会立即丢弃残余播放。
  void FinishTurn(bool normal) {
    ResetBargeProbe();
    barge_ended_ = finish_after_start_ = finish_after_cancel_ = false;
    if (!normal) audio_.DropPlayback();
    if (!AdvanceTurn()) return;
    ResetListener();
    state_ = State::kFollowUp; deadline_ = Clock::now() + std::chrono::seconds(3);
  }

  void ResumeAfterBarge() {
    audio_.DropPlayback(); audio_.Duck(false); finish_after_start_ = barge_ended_; finish_after_cancel_ = false;
    if (AdvanceTurn()) StartTurn();
  }

  void CompleteCancel() {
    if (finish_after_cancel_) FinishTurn(false); else ResumeAfterBarge();
  }

  // pre-roll 是 25 个固定槽的滚动窗；保存的是 AEC 后 PCM，发送时仍校验 capture sequence。
  void SavePreRoll(const CaptureFrame& frame) {
    pre_[(pre_head_ + pre_count_) % pre_.size()] = frame;
    if (pre_count_ < pre_.size()) ++pre_count_; else pre_head_ = (pre_head_ + 1U) % pre_.size();
  }

  bool SendAudio(const CaptureFrame& frame, bool end) {
    Pcm64Header h{};
    h.flags = static_cast<std::uint16_t>(
        (uplink_sequence_ == 0U ? 1U : 0U) | (end ? 2U : 0U));
    h.sequence = uplink_sequence_; h.timestamp_us = audio_origin_us_ + frame.sequence * 20000U; h.ids = ids_;
    if (!transport_->SendPcm64(
            h, reinterpret_cast<const std::uint8_t*>(frame.pcm.data()),
            frame.pcm.size() * sizeof(frame.pcm[0]))) return false;
    ++uplink_sequence_; ++turn_frames_; return true;
  }

  // turn.start 成功后先发送最老的 pre-roll，再发送实时帧，确保唤醒/打断不吃掉句首。
  bool StartTurn() {
    const std::size_t buffered = pre_count_;
    const std::uint64_t first_sequence = buffered == 0U
        ? 0U : pre_[pre_head_].sequence;
    for (std::size_t i = 1U; i < buffered; ++i) {
      const auto previous = pre_[(pre_head_ + i - 1U) % pre_.size()].sequence;
      const auto current = pre_[(pre_head_ + i) % pre_.size()].sequence;
      if (current != previous + 1U) {
        LoseConnection("pre-roll capture sequence gap"); return false;
      }
    }
    if (ids_.session == 0U ||
        !SendControl(ControlKind::kTurnStart, ids_)) {
      LoseConnection("turn.start failed"); return false;
    }
    uplink_sequence_ = turn_frames_ = 0U; state_ = State::kCapture;
    for (std::size_t i = 0U; i < pre_count_; ++i)
      if (!SendAudio(pre_[(pre_head_ + i) % pre_.size()], false)) {
        LoseConnection("pre-roll upload failed"); return false;
      }
    const std::uint64_t last_sequence = buffered == 0U
        ? 0U : pre_[(pre_head_ + buffered - 1U) % pre_.size()].sequence;
    pre_head_ = pre_count_ = 0U;
    std::cout << "boompi-client: VAD speech started; pre_roll_frames="
              << buffered << "; pre_roll_ms=" << buffered * 20U
              << "; capture_sequence=" << first_sequence << '-'
              << last_sequence << std::endl;
    return true;
  }

  void Commit(const CaptureFrame& frame) {
    if (!SendAudio(frame, true) ||
        !SendControl(ControlKind::kTurnCommit, ids_)) {
      LoseConnection("turn commit failed"); return;
    }
    state_ = State::kAwait; deadline_ = Clock::now() + std::chrono::seconds(30);
    std::cout << "boompi-client: VAD speech ended; voice turn committed" << std::endl;
  }

  // 首个候选立即静音；硬参考确认归零后清除200ms并重置VAD，再确认120ms。
  void HandleBarge(const CaptureFrame& frame, bool response_active) {
    if (!config_.barge_in_enabled) {
      ResetBargeProbe(); return;
    }
    if (barge_probe_ == BargeProbe::kIdle) {
      if (!frame.near_voice) return;
      near_frames_ = probe_frames_ = 0U; barge_probe_ = BargeProbe::kWaitReferenceLow;
      audio_.Duck(true); return;
    }
    if (barge_probe_ == BargeProbe::kWaitReferenceLow) {
      if (++probe_frames_ > kBargeReferenceWaitFrames) {
        ResetBargeProbe(); return;
      }
      near_frames_ = frame.reference_active ? 0U : near_frames_ + 1U;
      if (near_frames_ < kBargeReferenceLowFrames) return;
      near_frames_ = probe_frames_ = 0U; barge_probe_ = BargeProbe::kClear; return;
    }
    if (barge_probe_ == BargeProbe::kClear) {
      if (frame.reference_active) {
        ResetBargeProbe(); return;
      }
      if (++probe_frames_ < kBargeEchoClearFrames) return;
      near_frames_ = probe_frames_ = 0U; barge_probe_ = BargeProbe::kVerify;
      ResetListener(); return;
    }
    if (frame.reference_active || !frame.near_voice) {
      ResetBargeProbe(); return;
    }
    if (++near_frames_ != kBargeConfirmFrames) return;
    audio_.DropPlayback(); ResetBargeProbe();
    barge_ended_ = finish_after_cancel_ = false;
    if (!response_active) { ResumeAfterBarge(); return; }
    if (!SendControl(ControlKind::kResponseCancel, response_)) LoseConnection("response.cancel failed");
    else {
      state_ = State::kBarge;
      deadline_ = Clock::now() + std::chrono::seconds(3);
      std::cout << "boompi-client: barge-in confirmed" << std::endl;
    }
  }

  // 唯一的帧驱动入口；网络事件和取消 helper 也可转换状态，且必须共享同一 actor。
  void OnFrame(const CaptureFrame& frame) {
    if (frame.discontinuity) {
      pre_count_ = 0U;
      if (state_ == State::kBarge) LoseConnection("capture discontinuity while cancelling");
      else if (state_ == State::kCapture || state_ == State::kAwait ||
               state_ == State::kSpeak)
        AbortCurrent("capture discontinuity cancel failed");
      // kDrain 已消费 response.done，云端没有可取消对象，也不会再发送 cancelled。
      else if (state_ == State::kDrain) FinishTurn(false);
      else { audio_.DropPlayback(); state_ = State::kWake; ResetListener(); }
      return;
    }
    if (state_ != State::kCapture) SavePreRoll(frame);
    const auto now = Clock::now();
    if (ids_.session == 0U) {
      if (transport_ && now >= deadline_)
        LoseConnection("connect or hello timeout");
      return;
    }
    const bool timed_state = state_ == State::kWaitSpeech ||
        state_ == State::kAwait || state_ == State::kSpeak ||
        state_ == State::kBarge || state_ == State::kDrain ||
        state_ == State::kFollowUp;
    if (timed_state && now >= deadline_) {
      if (state_ == State::kWaitSpeech || state_ == State::kFollowUp) {
        state_ = State::kWake; pre_head_ = pre_count_ = 0U;
        ResetListener(); return;
      }
      if (state_ == State::kBarge) LoseConnection("voice cancel timed out");
      else if (state_ == State::kDrain) FinishTurn(false);
      else AbortCurrent("voice timeout cancel failed");
      return;
    }
    switch (state_) {
      case State::kWake:
        if (frame.wake) {
          state_ = State::kWaitSpeech; deadline_ = now + std::chrono::seconds(6);
          std::cout << "boompi-client: Snowboy wake detected" << std::endl;
          ResetListener();
        }
        break;
      case State::kWaitSpeech:
        if (frame.vad_started) StartTurn();
        break;
      case State::kFollowUp:
        if (!frame.near_voice) near_frames_ = 0U;
        else if (++near_frames_ >= kFollowUpNearFrames) StartTurn();
        break;
      case State::kCapture:
        if (finish_after_start_ || frame.vad_ended ||
            turn_frames_ >= 2999U) {
          finish_after_start_ = false; Commit(frame);
        }
        else if (!SendAudio(frame, false)) LoseConnection("uplink PCM send failed");
        break;
      case State::kSpeak: HandleBarge(frame, true); break;
      case State::kBarge: if (frame.vad_ended) barge_ended_ = true; break;
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
  const config::VoiceClientConfig& config_;
  AudioEngine audio_;
  std::unique_ptr<VoiceTransport> transport_;
  EventQueue events_;
  // 预卷帧与当前 turn 使用同一 capture sequence，便于发现 ALSA xrun 后的伪连续。
  std::array<CaptureFrame, kPreRollFrames> pre_{};
  std::size_t pre_head_{0U}, pre_count_{0U};
  WireIds ids_{}, response_{};
  State state_{State::kWake};
  Clock::time_point deadline_{}, next_connect_{};
  std::uint64_t audio_origin_us_{0U};
  std::uint32_t message_{1U}, uplink_sequence_{0U}, turn_frames_{0U};
  unsigned reconnect_attempt_{0U}, near_frames_{0U}, probe_frames_{0U};
  BargeProbe barge_probe_{BargeProbe::kIdle};
  bool hello_sent_{false}, barge_ended_{false}, finish_after_start_{false}, finish_after_cancel_{false};
  bool listener_failed_{false};
};

}  // namespace

Status RunVoiceClient(const config::VoiceClientConfig& config,
                      const volatile std::sig_atomic_t* stop) {
  try { VoiceClient client(config); return client.Run(stop); }
  catch (const std::bad_alloc&) {
    return Status::Error(StatusCode::kResourceExhausted,
                         "voice client allocation failed");
  }
  catch (...) { return Status::Error(StatusCode::kInternal, "voice client failed unexpectedly"); }
}

}  // namespace boompi::application
