/**
 * @file voice_client.cpp
 * @brief 对话流程：唤醒、收音、等待回答、播放和追问。
 *
 * 状态只由主线程修改。音频模块负责判断语句边界，网络模块负责连接和帧顺序。
 * generation 表示一次问答；打断时先换代，旧回答即使迟到也不能继续播放。
 */
#include "boompi/application/voice_client.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <limits>

#include "boompi/audio/voice_audio.h"
#include "boompi/network/voice_link.h"
#include "boompi/ui/device_ui.h"

namespace boompi::application {
namespace {
using Clock = std::chrono::steady_clock;
using ReadClock = Clock::time_point (*)();
using namespace std::chrono_literals;
using audio::AudioEvent;
using audio::AudioEventKind;
using network::LinkEvent;
using network::LinkEventKind;
using network::SendResult;
using ui::DeviceUiState;

enum class State { Offline, Idle, Listening, Uploading, Waiting, Speaking };

constexpr auto kWakeWindow = 6s;
constexpr auto kFollowUpWindow = 3s;
constexpr auto kInputLimit = 60s;
constexpr auto kResponseIdle = 30s;
constexpr auto kResponseLimit = 300s;
// 服务端发完后，最多还剩1.5s软件缓冲和80ms声卡缓冲，额外留出调度余量。
constexpr auto kDrainLimit = 3s;
constexpr std::uint32_t kMaximumInputFrames = 3000;  // 60s / 20ms

class VoiceApp final {
 public:
  // 测试传入可控时钟，以验证截止时刻；实际运行使用steady_clock，不受系统校时影响。
  VoiceApp(const config::VoiceClientConfig& config, std::string* error,
           ReadClock now = &Clock::now)
      : config_(config), error_(error), now_(now) {}

  bool Run(const volatile std::sig_atomic_t* stop) {
    if (!OpenModules()) {
      return false;
    }
    Enter(State::Offline);

    while ((stop == nullptr || *stop == 0) && !failed_) {
      OnTimeout();

      // 限制每轮网络消息数，持续下行时也要给采集和触摸留出处理机会。
      LinkEvent incoming;
      for (unsigned count = 0; count < 16 && link_.Poll(&incoming); ++count) {
        OnNetwork(incoming);
      }

      AudioEvent sound;
      const bool available = audio_.Poll(&sound, 20ms);
      // Poll可能等待20ms。先处理这段等待中发生的超时，再使用刚取出的语音。
      const bool expired = OnTimeout();
      if (available && !expired) {
        OnAudio(sound);
      }

      ui::UiAction action;
      if (ui_.Poll(&action)) {
        OnUser(action);
      }
      if (!audio_.healthy()) {
        Fail(audio_.last_error());
      }
    }
    return !failed_;
  }

  ~VoiceApp() {
    // 先停止网络投递，再回收音频和显示线程，避免关闭过程中继续接收回答。
    link_.Close();
    audio_.Close();
    ui_.Close();
  }

 private:
  bool OpenModules() {
    view_.volume = ui::DeviceUi::LoadVolume();
    if (!ui_.Open()) {
      std::fprintf(stderr, "boompi: display unavailable; voice continues\n");
    }
    if (!audio_.Open(view_.volume)) {
      return Fail(audio_.last_error());
    }

    network::LinkConfig link_config;
    link_config.device_id = config_.device_id;
    link_config.host = config_.server_ip;
    link_config.port = config_.server_port;
    link_config.spki = config_.server_spki_sha256;
    if (!link_.Open(link_config)) {
      return Fail("network startup failed");
    }
    return true;
  }

  /** @brief 切换对话状态，同时更新该状态的超时和显示。duration为0表示不计时。 */
  void Enter(State next, Clock::duration duration = Clock::duration::zero()) {
    state_ = next;
    deadline_ = Clock::time_point::max();
    if (duration != Clock::duration::zero()) {
      deadline_ = now_() + duration;
    }

    const char* name = "";
    switch (next) {
      case State::Offline:
        name = "offline";
        view_.state = DeviceUiState::kOffline;
        break;
      case State::Idle:
        name = "idle";
        view_.state = DeviceUiState::kIdle;
        break;
      case State::Listening:
        name = "listening";
        view_.state = DeviceUiState::kListening;
        break;
      case State::Uploading:
        name = "uploading";
        view_.state = DeviceUiState::kListening;
        break;
      case State::Waiting:
        name = "waiting";
        view_.state = DeviceUiState::kThinking;
        break;
      case State::Speaking:
        name = "speaking";
        view_.state = DeviceUiState::kSpeaking;
        break;
    }
    ui_.Show(view_);
    std::fprintf(stderr, "boompi: state=%s generation=%u\n", name, generation_);
  }

  bool HasActiveTurn() const {
    return state_ == State::Uploading || state_ == State::Waiting || state_ == State::Speaking;
  }

  bool Fail(const std::string& reason) {
    failed_ = true;
    if (error_ != nullptr) {
      *error_ = reason;
    }
    view_.state = DeviceUiState::kError;
    ui_.Show(view_);
    return false;
  }

  bool NextGeneration() {
    // 不允许序号回绕后与本连接的旧回复重名。
    if (generation_ == std::numeric_limits<std::uint32_t>::max()) {
      return Fail("generation exhausted; restart client");
    }
    ++generation_;
    return true;
  }

  void Listen(bool follow_up) {
    audio_.CancelInput();
    if (!audio_.Listen(follow_up)) {
      Fail(audio_.last_error());
      return;
    }
    Enter(State::Listening, follow_up ? kFollowUpWindow : kWakeWindow);
  }

  void Offline() {
    audio_.StopPlayback();
    audio_.CancelInput();
    view_.ClearText();
    Enter(State::Offline);
  }

  /**
   * @brief 停止当前问答；retract表示撤回用户尚未听完的回答历史。
   *
   * 输入缺帧或超时时不能发送END，否则服务端会把残缺语句当成完整问题。
   * STOP使用新的generation，失败后退回离线，等待网络模块重新连接。
   */
  void StopTurn(bool retract) {
    audio_.StopPlayback();
    audio_.CancelInput();
    view_.ClearText();
    if (!NextGeneration()) {
      return;
    }
    if (!link_.Stop(generation_, retract)) {
      Offline();
      return;
    }
    Listen(/*follow_up=*/true);
  }

  void BeginUpload(bool supersede) {
    if (!NextGeneration()) {
      return;
    }
    sent_frames_ = 0;
    supersede_ = supersede;
    view_.ClearText();
    Enter(State::Uploading, kInputLimit);
  }

  /** @brief 按原顺序上传语音；只有最后一帧成功入队，才开始等待回答。 */
  void SendInputFrame(const AudioEvent& event) {
    if (state_ != State::Uploading) {
      return;
    }
    const bool first_frame = sent_frames_ == 0;
    const bool last_frame = event.end || sent_frames_ + 1 >= kMaximumInputFrames;
    const bool replace_answer = first_frame && supersede_;
    const SendResult result =
        link_.SendAudio(generation_, event.pcm.data(), first_frame, last_frame, replace_answer);
    if (result == SendResult::Backpressure) {
      StopTurn(/*retract=*/false);
      return;
    }
    if (result != SendResult::Ok) {
      Offline();
      return;
    }

    ++sent_frames_;
    if (last_frame) {
      audio_.CancelInput();
      response_limit_ = now_() + kResponseLimit;
      Enter(State::Waiting, kResponseIdle);
    }
  }

  void HandleAudioFault() {
    if (!audio_.healthy()) {
      Fail(audio_.last_error());
    } else if (HasActiveTurn()) {
      StopTurn(state_ == State::Speaking);
    } else if (state_ != State::Offline) {
      audio_.CancelInput();
      Enter(State::Idle);
    }
  }

  void OnAudio(const AudioEvent& event) {
    switch (event.kind) {
      case AudioEventKind::Wake:
        if (state_ == State::Idle) {
          Listen(/*follow_up=*/false);
        }
        break;
      case AudioEventKind::SpeechStart:
        if (state_ == State::Listening) {
          BeginUpload(/*supersede=*/false);
        }
        break;
      case AudioEventKind::Barge:
        if (state_ == State::Speaking) {
          BeginUpload(/*supersede=*/true);
        }
        break;
      case AudioEventKind::Pcm:
        SendInputFrame(event);
        break;
      case AudioEventKind::PlaybackDone:
        // 旧播放线程的完成通知，不能结束已经开始的新回答。
        if (event.generation == generation_ && state_ == State::Speaking) {
          Listen(/*follow_up=*/true);
        }
        break;
      case AudioEventKind::Fault:
        HandleAudioFault();
        break;
    }
  }

  void PlayReplyFrame(const LinkEvent& event) {
    const bool queued = audio_.Play(generation_, event.audio.data(), event.audio_size,
                                    event.sequence, event.start, event.end);
    if (!queued) {
      if (!audio_.healthy()) {
        Fail(audio_.last_error());
      } else {
        StopTurn(/*retract=*/true);
      }
      return;
    }
    if (state_ == State::Waiting) {
      Enter(State::Speaking, kResponseIdle);
    }

    // 收到END只缩短尾播等待；真正离开Speaking仍由PlaybackDone决定。
    const auto remaining = event.end ? kDrainLimit : kResponseIdle;
    deadline_ = std::min(now_() + remaining, response_limit_);
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
    if (event.generation != generation_ || state_ == State::Offline) {
      return;
    }
    // 服务端也可能在上传尚未结束时失败，此时应立即停止这轮输入。
    if (event.kind == LinkEventKind::Error) {
      std::fprintf(stderr, "boompi: reply failed; code=%s\n", event.code.c_str());
      if (HasActiveTurn()) {
        StopTurn(state_ == State::Speaking);
      }
      return;
    }
    if (state_ != State::Waiting && state_ != State::Speaking) {
      return;
    }

    switch (event.kind) {
      case LinkEventKind::Text:
        view_.AppendText(event.text);
        ui_.Show(view_);
        deadline_ = std::min(now_() + kResponseIdle, response_limit_);
        break;
      case LinkEventKind::Audio:
        PlayReplyFrame(event);
        break;
      case LinkEventKind::Done:
        // 纯文本回答没有PlaybackDone；带音频的回答要等声卡尾播结束。
        if (state_ == State::Waiting) {
          Listen(/*follow_up=*/true);
        }
        break;
      default:
        break;
    }
  }

  void OnUser(const ui::UiAction& action) {
    if (action.kind == ui::UiActionKind::Volume) {
      view_.volume = std::min<std::uint8_t>(100, action.volume);
      audio_.SetVolume(view_.volume);
      ui_.Show(view_);
    } else if (action.kind == ui::UiActionKind::Interrupt) {
      if (state_ == State::Speaking || state_ == State::Waiting) {
        StopTurn(/*retract=*/true);
      }
    } else if (state_ == State::Idle) {
      Listen(/*follow_up=*/false);
    }
  }

  /** @return 本次调用是否因到期改变了状态。 */
  bool OnTimeout() {
    if (now_() < deadline_) {
      return false;
    }
    if (state_ == State::Listening) {
      audio_.CancelInput();
      Enter(State::Idle);
    } else if (HasActiveTurn()) {
      StopTurn(state_ == State::Speaking);
    } else {
      return false;
    }
    return true;
  }

  const config::VoiceClientConfig& config_;
  std::string* error_;
  ReadClock now_;
  audio::VoiceAudio audio_;
  network::VoiceLink link_;
  ui::DeviceUi ui_;
  ui::UiView view_;

  State state_{State::Offline};
  Clock::time_point deadline_{Clock::time_point::max()};
  Clock::time_point response_limit_{};
  std::uint32_t generation_{0};
  std::uint32_t sent_frames_{0};
  bool supersede_{false};
  bool failed_{false};
};
}  // namespace

bool RunVoiceClient(const config::VoiceClientConfig& config,
                    const volatile std::sig_atomic_t* stop, std::string* error) {
  try {
    return VoiceApp(config, error).Run(stop);
  } catch (...) {
    if (error != nullptr) {
      *error = "client initialization or resource allocation failed";
    }
    return false;
  }
}
}  // namespace boompi::application
