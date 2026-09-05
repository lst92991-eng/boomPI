/**
 * @file voice_client.cpp
 * @brief 课程主线：离线 → 空闲 → 聆听 → 上传 → 等待 → 播放。
 *
 * 本线程只决定用户体验。VoiceAudio 交付完整语句与打断事件，VoiceLink 维护连接
 * 和 v2 帧顺序。一次打断直接开启新 generation，不等待旧回复取消确认。
 */
#include "boompi/application/voice_client.h"
#include "boompi/audio/voice_audio.h"
#include "boompi/network/voice_link.h"
#include "boompi/ui/device_ui.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <limits>

namespace boompi::application {
namespace {
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
using audio::AudioEvent;
using audio::AudioEventKind;
using network::LinkEvent;
using network::LinkEventKind;
using network::SendResult;
using ui::DeviceUiState;

enum class State { Offline, Idle, Listening, Uploading, Waiting, Speaking };

// 时间是产品策略，不是学生标定参数。所有测试使用同一策略和可控时钟。
constexpr auto kWakeWindow = 6s;
constexpr auto kFollowUpWindow = 3s;
constexpr auto kInputLimit = 60s;
constexpr auto kResponseIdle = 30s;
constexpr auto kResponseLimit = 300s;
constexpr auto kDrainLimit = 3s;  // 1.5 s TTS ring + ALSA 尾播 + 调度余量。
constexpr std::uint32_t kMaximumInputFrames = 3000;  // 60 s / 20 ms。

class VoiceApp final {
 public:
  VoiceApp(const config::VoiceClientConfig& config, std::string* error,
           Clock::time_point (*now)() = &Clock::now)
      : config_(config), error_(error), now_(now) {}

  bool Run(const volatile std::sig_atomic_t* stop) {
    view_.volume = ui::DeviceUi::LoadVolume();
    if (!ui_.Open()) std::fprintf(stderr, "boompi: display unavailable; voice continues\n");
    if (!audio_.Open(view_.volume)) return Fail(audio_.last_error());
    network::LinkConfig link_config;
    link_config.device_id = config_.device_id;
    link_config.host = config_.server_ip;
    link_config.port = config_.server_port;
    link_config.spki = config_.server_spki_sha256;
    if (!link_.Open(link_config)) return Fail("network startup failed");
    Enter(State::Offline);
    while ((stop == nullptr || *stop == 0) && !failed_) {
      OnTimeout();
      LinkEvent incoming;
      for (unsigned i = 0; i < 16 && link_.Poll(&incoming); ++i) OnNetwork(incoming);
      AudioEvent sound;
      const bool available = audio_.Poll(&sound, 20ms);
      // 等待音频期间到期的语句先封口；刚出队的旧语音不能重新打开这次上传。
      const bool expired = OnTimeout();
      if (available && !expired) OnAudio(sound);
      ui::UiAction action;
      if (ui_.Poll(&action)) OnUser(action);
      if (!audio_.healthy()) Fail(audio_.last_error());
    }
    return !failed_;
  }

  ~VoiceApp() {
    link_.Close();
    audio_.Close();
    ui_.Close();
  }

 private:
  void Enter(State next, Clock::duration duration = Clock::duration::zero()) {
    static constexpr const char* names[] =
        {"offline", "idle", "listening", "uploading", "waiting", "speaking"};
    static constexpr DeviceUiState displays[] = {
        DeviceUiState::kOffline, DeviceUiState::kIdle, DeviceUiState::kListening,
        DeviceUiState::kListening, DeviceUiState::kThinking, DeviceUiState::kSpeaking};
    state_ = next;
    deadline_ = duration == Clock::duration::zero() ? Clock::time_point::max() : now_() + duration;
    view_.state = displays[static_cast<unsigned>(next)];
    ui_.Show(view_);
    std::fprintf(stderr, "boompi: state=%s generation=%u\n",
                 names[static_cast<unsigned>(next)], generation_);
  }

  bool Fail(const std::string& reason) {
    failed_ = true;
    if (error_ != nullptr) *error_ = reason;
    view_.state = DeviceUiState::kError;
    ui_.Show(view_);
    return false;
  }

  bool NextGeneration() {
    if (generation_ == std::numeric_limits<std::uint32_t>::max())
      return Fail("generation exhausted; restart client");
    ++generation_;
    return true;
  }

  void Listen(bool follow_up) {
    audio_.CancelInput();
    if (!audio_.Listen(follow_up)) { Fail(audio_.last_error()); return; }
    Enter(State::Listening, follow_up ? kFollowUpWindow : kWakeWindow);
  }

  void Offline() {
    audio_.StopPlayback();
    audio_.CancelInput();
    view_.ClearText();
    Enter(State::Offline);
  }

  // 没有完整语句时必须 STOP，不能用 END 把残缺语音交给 ASR。
  void StopTurn(bool retract) {
    audio_.StopPlayback();
    audio_.CancelInput();
    view_.ClearText();
    if (!NextGeneration()) return;
    if (!link_.Stop(generation_, retract)) { Offline(); return; }
    Listen(true);
  }

  void BeginUpload(bool supersede) {
    if (!NextGeneration()) return;
    sent_frames_ = 0;
    supersede_ = supersede;
    view_.ClearText();
    Enter(State::Uploading, kInputLimit);
  }

  void OnAudio(const AudioEvent& event) {
    switch (event.kind) {
      case AudioEventKind::Wake:
        if (state_ == State::Idle) Listen(false);
        break;
      case AudioEventKind::SpeechStart:
        if (state_ == State::Listening) BeginUpload(false);
        break;
      case AudioEventKind::Barge:
        if (state_ == State::Speaking) BeginUpload(true);
        break;
      case AudioEventKind::Pcm: {
        if (state_ != State::Uploading) break;
        const bool end = event.end || sent_frames_ + 1 >= kMaximumInputFrames;
        const auto result = link_.SendAudio(generation_, event.pcm.data(),
                                            sent_frames_ == 0, end,
                                            sent_frames_ == 0 && supersede_);
        if (result == SendResult::Backpressure) { StopTurn(false); break; }
        if (result != SendResult::Ok) { Offline(); break; }
        ++sent_frames_;
        if (end) {
          audio_.CancelInput();
          response_limit_ = now_() + kResponseLimit;
          Enter(State::Waiting, kResponseIdle);
        }
        break;
      }
      case AudioEventKind::PlaybackDone:
        if (event.generation == generation_ && state_ == State::Speaking) Listen(true);
        break;
      case AudioEventKind::Fault:
        if (!audio_.healthy()) Fail(audio_.last_error());
        else if (state_ == State::Uploading || state_ == State::Speaking ||
                 state_ == State::Waiting) StopTurn(state_ == State::Speaking);
        else if (state_ != State::Offline) {
          audio_.CancelInput();
          Enter(State::Idle);
        }
        break;
      case AudioEventKind::SpeechEnd: break;  // 最后一个 PCM 的 end 已经提交语句。
    }
  }

  void OnNetwork(const LinkEvent& event) {
    if (event.kind == LinkEventKind::Online) {
      audio_.CancelInput();
      view_.ClearText();
      Enter(State::Idle);
      return;
    }
    if (event.kind == LinkEventKind::Offline) {
      std::fprintf(stderr, "boompi: offline; stage=%s\n", event.code.c_str());
      Offline();
      return;
    }
    if (event.generation != generation_ || state_ == State::Offline) return;
    if (event.kind == LinkEventKind::Error) {
      std::fprintf(stderr, "boompi: reply failed; code=%s\n", event.code.c_str());
      if (state_ == State::Uploading || state_ == State::Waiting || state_ == State::Speaking)
        StopTurn(state_ == State::Speaking);
      return;
    }
    const bool responding = state_ == State::Waiting || state_ == State::Speaking;
    if (!responding) return;
    switch (event.kind) {
      case LinkEventKind::Text:
        view_.AppendText(event.text);
        ui_.Show(view_);
        deadline_ = std::min(now_() + kResponseIdle, response_limit_);
        break;
      case LinkEventKind::Audio:
        if (!audio_.Play(generation_, event.audio.data(), event.audio_size,
                         event.sequence, event.start, event.end)) {
          if (!audio_.healthy()) Fail(audio_.last_error());
          else StopTurn(true);
          return;
        }
        if (state_ == State::Waiting) Enter(State::Speaking, kResponseIdle);
        deadline_ = std::min(now_() + (event.end ? kDrainLimit : kResponseIdle), response_limit_);
        break;
      case LinkEventKind::Done:
        // 纯文本回答没有 PlaybackDone；有音频则等物理尾播完成。
        if (state_ == State::Waiting) Listen(true);
        break;
      default: break;
    }
  }

  void OnUser(const ui::UiAction& action) {
    if (action.kind == ui::UiActionKind::Volume) {
      view_.volume = std::min<std::uint8_t>(100, action.volume);
      audio_.SetVolume(view_.volume);
      ui_.Show(view_);
    } else if (action.kind == ui::UiActionKind::Interrupt) {
      if (state_ == State::Speaking || state_ == State::Waiting) StopTurn(true);
    } else if (state_ == State::Idle) Listen(false);
  }

  bool OnTimeout() {
    if (now_() < deadline_) return false;
    if (state_ == State::Listening) {
      audio_.CancelInput();
      Enter(State::Idle);
    } else if (state_ == State::Uploading || state_ == State::Waiting ||
               state_ == State::Speaking) {
      StopTurn(state_ == State::Speaking);
    } else return false;
    return true;
  }

  const config::VoiceClientConfig& config_;
  std::string* error_;
  Clock::time_point (*now_)();
  audio::VoiceAudio audio_;
  network::VoiceLink link_;
  ui::DeviceUi ui_;
  ui::UiView view_;
  State state_{State::Offline};
  Clock::time_point deadline_{Clock::time_point::max()}, response_limit_{};
  std::uint32_t generation_{0}, sent_frames_{0};
  bool supersede_{false}, failed_{false};
};
}  // namespace

bool RunVoiceClient(const config::VoiceClientConfig& config,
                    const volatile std::sig_atomic_t* stop, std::string* error) {
  try { return VoiceApp(config, error).Run(stop); }
  catch (...) {
    if (error != nullptr) *error = "client initialization or resource allocation failed";
    return false;
  }
}
}  // namespace boompi::application
