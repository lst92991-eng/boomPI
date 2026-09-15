/** @file voice_client.cpp
 * @brief 正常问答与插话的主流程，所有状态只由应用线程修改。
 * App_Init 初始化；App_Process 顺序处理回复、录音和触摸；App_Close 有序退出。
 * 音频负责语句和播放事实，网络负责传输，应用负责轮次及下一步动作。
 */
#include "boompi/application/voice_client.h"

#include <algorithm>
#include <cstdio>
#include <limits>
#include <vector>

#include "boompi/audio/voice_audio.h"
#include "boompi/network/voice_link.h"
#include "boompi/ui/device_ui.h"

namespace {
namespace audio = boompi::audio;
namespace network = boompi::network;
namespace ui = boompi::ui;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
using audio::AudioEvent;
using audio::AudioEventKind;
using audio::ListenMode;
using network::LinkEvent;
using network::LinkEventKind;
using network::SendResult;
using ui::DeviceUiState;

constexpr auto kWakeWindow = 6s;
constexpr auto kFollowUpWindow = 3s;
constexpr auto kInputLimit = 60s;
constexpr auto kResponseIdle = 30s;
constexpr auto kResponseLimit = 300s;
// 网络 END 后还要等软件队列和 ALSA 尾播，给固定容量缓冲留出时间。
constexpr auto kDrainLimit = 3s;
constexpr std::uint32_t kMaximumInputFrames = audio::VoiceFrameContract::FramesForMs(60000U);
// 应用状态只由主线程操作，后台任务通过各模块自己的队列交接。
enum class State { Offline, Idle, Listening, Uploading, Waiting, Speaking };
audio::VoiceAudio audio_;
network::VoiceLink link_;
ui::DeviceUi ui_;
ui::UiView view_;
std::vector<AudioEvent> speech_events_;
AppReadClock now_ = &Clock::now;
State state_ = State::Offline;
Clock::time_point deadline_ = Clock::time_point::max();
Clock::time_point response_limit_{};
std::uint32_t generation_ = 0;
std::uint32_t sent_frames_ = 0;
bool supersede_ = false;
// 网络完成与声卡完成可能先后交换；无音频时本地天然已完成。
bool reply_done_ = false;
bool playback_done_ = true;
char error_[192]{};

void App_ReceiveReplyAndPlayAudio();
void App_ReadSpeechAndUpload();
void App_ReadUserAction();
void App_UploadSpeechFrame(const AudioEvent& frame);
void App_QueueReplyAudio(const LinkEvent& reply);
void App_Enter(State next, Clock::duration duration = Clock::duration::zero());
bool App_HasActiveTurn();
bool App_Fail(const char* reason);
bool App_NextGeneration();
void App_WaitForSpeech(ListenMode mode);
void App_GoOffline();
void App_StopAndListen(bool retract_history);
void App_BeginUpload(bool supersede);
void App_HandleAudioFault();
bool App_CheckTimeout();
}  // namespace

// UI 失败仍可进行语音问答；音频或网络启动失败交给 main 退出。
bool App_Init(const boompi::config::VoiceClientConfig& config, AppReadClock clock) {
  now_ = clock;
  error_[0] = '\0';
  view_ = {};
  speech_events_.clear();
  state_ = State::Offline;
  deadline_ = Clock::time_point::max();
  response_limit_ = {};
  generation_ = sent_frames_ = 0;
  supersede_ = false;
  reply_done_ = false;
  playback_done_ = true;
  try {
    // 1. 读取音量，打开屏幕和触摸。显示失败时仍可进行语音问答。
    view_.volume = ui::DeviceUi::LoadVolume();
    if (!ui_.Open()) {
      std::fprintf(stderr, "boompi: display unavailable; voice continues\n");
    }
    // 2. 打开声卡与算法，启动持续采集和播放任务。
    if (!audio_.Open(view_.volume)) {
      return App_Fail(audio_.LastError().c_str());
    }

    // 3. 启动网络任务，后续收到 Online 才接受唤醒。
    if (!link_.Open(config)) {
      return App_Fail("network startup failed");
    }
    App_Enter(State::Offline);
    return true;
  } catch (...) {
    return App_Fail("application initialization failed");
  }
}

// 每次处理一轮。main 负责循环和退出信号，模块只处理各自的数据。
bool App_Process() {
  try {
    App_CheckTimeout();

    // 1. 接收服务器回复，更新字幕，把回复音频送入播放队列。
    App_ReceiveReplyAndPlayAudio();

    // 2. 取出录音判定与 PCM，按开口、声音、句尾的顺序上传。
    App_ReadSpeechAndUpload();

    // 3. 读取触摸操作，更新音量或停止当前回答。
    App_ReadUserAction();

    if (!audio_.IsHealthy()) {
      App_Fail(audio_.LastError().c_str());
    }
    return error_[0] == '\0';
  } catch (...) {
    return App_Fail("application processing failed");
  }
}

void App_Close() noexcept {
  link_.Close();
  audio_.Close();
  ui_.Close();
}

const char* App_GetError() noexcept {
  return error_;
}

namespace {
// 服务器消息 → 当前轮校验 → 字幕显示或音频播放。
void App_ReceiveReplyAndPlayAudio() {
  // 每轮最多取 16 条，持续下行时也给录音和触摸留出处理机会。
  LinkEvent event;
  for (unsigned count = 0; count < 16 && link_.PollEvent(&event); ++count) {
    if (event.kind == LinkEventKind::Online) {
      audio_.CancelInput();
      view_.ClearText();
      App_Enter(State::Idle);
      continue;
    }
    if (event.kind == LinkEventKind::Offline) {
      std::fprintf(stderr, "boompi: offline; stage=%s\n", event.code.c_str());
      App_GoOffline();
      continue;
    }
    if (event.generation != generation_ || state_ == State::Offline) {
      continue;
    }
    // 服务端也可能在上传尚未结束时失败，此时应立即停止这轮输入。
    if (event.kind == LinkEventKind::Error) {
      std::fprintf(stderr, "boompi: reply failed; code=%s\n", event.code.c_str());
      if (App_HasActiveTurn()) {
        App_StopAndListen(state_ == State::Speaking);
      }
      continue;
    }
    if (state_ != State::Waiting && state_ != State::Speaking) {
      continue;
    }

    switch (event.kind) {
      case LinkEventKind::Text:
        view_.AppendText(event.text);
        ui_.Show(view_);
        deadline_ = std::min(now_() + kResponseIdle, response_limit_);
        break;
      case LinkEventKind::Audio:
        App_QueueReplyAudio(event);
        break;
      case LinkEventKind::Done:
        reply_done_ = true;
        // 无音频的回答没有 PlaybackDone；两种完成顺序都要等齐。
        if (playback_done_) {
          App_WaitForSpeech(ListenMode::FollowUp);
        }
        break;
      default:
        break;
    }
  }
}

// 音频入队后继续等待实际尾播；网络 END 不等于扬声器播完。
void App_QueueReplyAudio(const LinkEvent& event) {
  const bool queued = audio_.Play(generation_, event.audio.data(), event.audio_size,
                                  event.sequence, event.start, event.end);
  if (!queued) {
    if (!audio_.IsHealthy()) {
      App_Fail(audio_.LastError().c_str());
    } else {
      App_StopAndListen(/*retract_history=*/true);
    }
    return;
  }
  if (state_ == State::Waiting) {
    playback_done_ = false;
    App_Enter(State::Speaking, kResponseIdle);
  }

  // 收到END只缩短尾播等待；真正离开Speaking仍由PlaybackDone决定。
  const auto remaining = event.end ? kDrainLimit : kResponseIdle;
  deadline_ = std::min(now_() + remaining, response_limit_);
}

// 录音 → 开口/插话判定 → PCM 上传。开始事件和句首在同一轮处理。
void App_ReadSpeechAndUpload() {
  // 1. 从音频模块取出本轮结果，等待期间不占用采集线程。
  audio_.ProcessEvents(speech_events_, 20ms);
  if (App_CheckTimeout()) {
    return;  // 等待期间已到期，这批声音不再作为新问题提交。
  }

  // 2. 先处理开始事件，再按原顺序上传 PCM。
  for (const AudioEvent& event : speech_events_) {
    switch (event.kind) {
      case AudioEventKind::Wake:
        if (state_ == State::Idle) {
          App_WaitForSpeech(ListenMode::Wake);
        }
        break;
      case AudioEventKind::SpeechStart:
        if (state_ == State::Listening) {
          App_BeginUpload(/*supersede=*/false);
        }
        break;
      case AudioEventKind::Barge:
        if (event.generation == generation_ && state_ == State::Speaking) {
          App_BeginUpload(/*supersede=*/true);
        }
        break;
      case AudioEventKind::Pcm:
        App_UploadSpeechFrame(event);
        break;
      case AudioEventKind::PlaybackDone:
        // 旧播放线程的完成通知，不能结束已经开始的新回答。
        if (event.generation == generation_ && state_ == State::Speaking) {
          playback_done_ = true;
          if (reply_done_) {
            App_WaitForSpeech(ListenMode::FollowUp);
          }
        }
        break;
      case AudioEventKind::Fault:
        App_HandleAudioFault();
        break;
    }
    // 3. 句尾、取消或故障后放弃本批剩余声音，防止串到下一轮。
    if (error_[0] != '\0' || state_ != State::Uploading) {
      break;
    }
  }
}

// 首帧 START，末帧 END；背压取消整轮，不跳过中间 PCM。
void App_UploadSpeechFrame(const AudioEvent& event) {
  if (state_ != State::Uploading) {
    return;
  }
  const bool first_frame = sent_frames_ == 0;
  const bool last_frame = event.end || sent_frames_ + 1 >= kMaximumInputFrames;
  const bool replace_answer = first_frame && supersede_;
  const SendResult result =
      link_.SendAudio(generation_, event.pcm.data(), first_frame, last_frame, replace_answer);
  if (result == SendResult::Backpressure) {
    App_StopAndListen(/*retract_history=*/false);
    return;
  }
  if (result != SendResult::Ok) {
    App_GoOffline();
    return;
  }

  ++sent_frames_;
  if (last_frame) {
    audio_.CancelInput();
    response_limit_ = now_() + kResponseLimit;
    App_Enter(State::Waiting, kResponseIdle);
  }
}

// UI 只交付用户意图，主线程决定是否改变对话流程。
void App_ReadUserAction() {
  ui::UiAction action;
  if (!ui_.PollAction(&action)) {
    return;
  }
  if (action.kind == ui::UiActionKind::Volume) {
    view_.volume = std::min<std::uint8_t>(100, action.volume);
    audio_.SetVolume(view_.volume);
    ui_.Show(view_);
  } else if (action.kind == ui::UiActionKind::Interrupt) {
    if (state_ == State::Speaking || state_ == State::Waiting) {
      App_StopAndListen(/*retract_history=*/true);
    }
  } else if (state_ == State::Idle) {
    App_WaitForSpeech(ListenMode::Wake);
  }
}

// 只等有效开口，收到 SpeechStart 后才分配轮次。
void App_WaitForSpeech(ListenMode mode) {
  // Listen 在采集帧边界复位检测并清除旧句首，无须先重复 CancelInput。
  if (!audio_.Listen(mode)) {
    App_Fail(audio_.LastError().c_str());
    return;
  }
  App_Enter(State::Listening, mode == ListenMode::FollowUp ? kFollowUpWindow : kWakeWindow);
}

// SpeechStart 与 Barge 共用上传流程，插话只在首帧携带替换标记。
void App_BeginUpload(bool supersede) {
  if (!App_NextGeneration()) {
    return;
  }
  sent_frames_ = 0;
  supersede_ = supersede;
  reply_done_ = false;
  playback_done_ = true;
  view_.ClearText();
  App_Enter(State::Uploading, kInputLimit);
}

// 缺帧或超时使用 STOP 取消，不能伪造 END 提交残缺发言。
void App_StopAndListen(bool retract_history) {
  audio_.StopPlayback();
  audio_.CancelInput();
  view_.ClearText();
  if (!App_NextGeneration()) {
    return;
  }
  if (!link_.Stop(generation_, retract_history)) {
    App_GoOffline();
    return;
  }
  App_WaitForSpeech(ListenMode::FollowUp);
}

// 断线时丢弃当前问题和回答；重连后从新的发言开始。
void App_GoOffline() {
  audio_.StopPlayback();
  audio_.CancelInput();
  view_.ClearText();
  App_Enter(State::Offline);
}

// 状态、截止时间与显示在同一处更新；Uploading 在界面上仍显示聆听。
void App_Enter(State next, Clock::duration duration) {
  state_ = next;
  deadline_ = Clock::time_point::max();
  if (duration != Clock::duration::zero()) {
    deadline_ = now_() + duration;
  }

  switch (next) {
    case State::Offline:
      view_.state = DeviceUiState::Offline;
      break;
    case State::Idle:
      view_.state = DeviceUiState::Idle;
      break;
    case State::Listening:
    case State::Uploading:
      view_.state = DeviceUiState::Listening;
      break;
    case State::Waiting:
      view_.state = DeviceUiState::Thinking;
      break;
    case State::Speaking:
      view_.state = DeviceUiState::Speaking;
      break;
  }
  ui_.Show(view_);
}

// 音频等待可能跨过截止时刻；到期后不能使用刚收到的过期发言。
bool App_CheckTimeout() {
  if (now_() < deadline_) {
    return false;
  }
  if (state_ == State::Listening) {
    audio_.CancelInput();
    App_Enter(State::Idle);
  } else if (App_HasActiveTurn()) {
    App_StopAndListen(state_ == State::Speaking);
  } else {
    return false;
  }
  return true;
}

// 设备失效退出；可恢复的语句/播放失败只结束当前轮。
void App_HandleAudioFault() {
  if (!audio_.IsHealthy()) {
    App_Fail(audio_.LastError().c_str());
  } else if (App_HasActiveTurn()) {
    App_StopAndListen(state_ == State::Speaking);
  } else if (state_ != State::Offline) {
    audio_.CancelInput();
    App_Enter(State::Idle);
  }
}

// 轮次不可回绕，旧数据不能取得新的有效身份。
bool App_NextGeneration() {
  // 不允许序号回绕后与本连接的旧回复重名。
  if (generation_ == std::numeric_limits<std::uint32_t>::max()) {
    return App_Fail("generation exhausted; restart client");
  }
  ++generation_;
  return true;
}

bool App_HasActiveTurn() {
  return state_ == State::Uploading || state_ == State::Waiting || state_ == State::Speaking;
}

bool App_Fail(const char* reason) {
  std::snprintf(error_, sizeof(error_), "%s", reason);
  view_.state = DeviceUiState::Error;
  ui_.Show(view_);
  return false;
}

}  // namespace
