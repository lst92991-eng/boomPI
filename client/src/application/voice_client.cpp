#include "boompi/application/voice_client.h"

#include <algorithm>
#include <cstdio>
#include <limits>

#include "boompi/audio/audio_capture.h"
#include "boompi/audio/playback.h"
#include "boompi/audio/speech.h"
#include "boompi/network/voice_net.h"
#include "boompi/ui/device_ui.h"

namespace {
namespace audio_capture = boompi::audio_capture;
namespace playback = boompi::playback;
namespace speech = boompi::speech;
namespace voice_net = boompi::voice_net;
namespace ui = boompi::ui;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
using speech::ListenMode;
using ui::DeviceUiState;
using voice_net::LinkEvent;
using voice_net::LinkEventKind;
using voice_net::SendResult;

constexpr auto kWakeWindow = 6s;
constexpr auto kFollowUpWindow = 3s;
constexpr auto kInputLimit = 60s;
constexpr auto kResponseIdle = 30s;
constexpr auto kResponseLimit = 300s;
constexpr auto kDrainLimit = 3s;
constexpr auto kMaximumInputFrames = boompi::audio::VoiceFrameContract::FramesForMs(60000);

// 聆听尚未START，上传已经START；保留这个区别，不以几个互相依赖的bool代替。
enum class State { Offline, Idle, Listening, Uploading, WaitingReply, Speaking };
State state_ = State::Offline;
ui::DeviceUi ui_;
ui::UiView view_;
AppReadClock now_ = &Clock::now;
Clock::time_point deadline_ = Clock::time_point::max();
Clock::time_point response_limit_{};
std::uint32_t generation_ = 0;
std::uint32_t sent_frames_ = 0;
char error_[192]{};

bool App_Fail(const char* reason) {
  std::snprintf(error_, sizeof(error_), "%s", reason);
  view_.state = DeviceUiState::Error;
  ui_.Show(view_);
  return false;
}

void App_Enter(State next, Clock::duration duration = Clock::duration::zero()) {
  state_ = next;
  deadline_ =
      duration == Clock::duration::zero() ? Clock::time_point::max() : now_() + duration;
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
    case State::WaitingReply:
      view_.state = DeviceUiState::Thinking;
      break;
    case State::Speaking:
      view_.state = DeviceUiState::Speaking;
      break;
  }
  ui_.Show(view_);
}

bool App_NextGeneration() {
  if (generation_ == std::numeric_limits<std::uint32_t>::max()) {
    return App_Fail("generation exhausted; restart client");
  }
  ++generation_;
  return true;
}

bool App_HasActiveTurn() {
  return state_ == State::Uploading || state_ == State::WaitingReply ||
         state_ == State::Speaking;
}

void App_WaitForSpeech(ListenMode mode) {
  if (!audio_capture::reset_listener()) {
    App_Fail(audio_capture::error().c_str());
    return;
  }
  speech::listen(mode);
  playback::set_scale(1.0F);
  App_Enter(State::Listening, mode == ListenMode::FollowUp ? kFollowUpWindow : kWakeWindow);
}

void App_GoOffline() {
  playback::cancel();
  speech::reset();
  view_.ClearText();
  App_Enter(State::Offline);
}

void App_StopAndListen(bool retract) {
  playback::cancel();
  speech::reset();
  view_.ClearText();
  if (!App_NextGeneration()) {
    return;
  }
  if (!voice_net::cancel(generation_, retract)) {
    App_GoOffline();
    return;
  }
  App_WaitForSpeech(ListenMode::FollowUp);
}

bool App_Sent(SendResult result) {
  if (result == SendResult::Ok) {
    return true;
  }
  if (result == SendResult::Backpressure) {
    App_StopAndListen(false);
  } else {
    App_GoOffline();
  }
  return false;
}

bool App_CheckTimeout() {
  if (now_() < deadline_) {
    return false;
  }
  if (state_ == State::Listening) {
    speech::reset();
    App_Enter(State::Idle);
  } else if (App_HasActiveTurn()) {
    App_StopAndListen(state_ == State::Speaking);
  } else {
    return false;
  }
  return true;
}

// 下行直接进入播放模块，不再经过语句模块、Engine和Backend逐层转交PCM。
void App_ReceiveReply() {
  LinkEvent event;
  for (unsigned count = 0; count < 16 && voice_net::poll(&event); ++count) {
    if (event.kind == LinkEventKind::Online) {
      speech::reset();
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
    if (event.kind == LinkEventKind::Error) {
      std::fprintf(stderr, "boompi: reply failed; code=%s\n", event.code.c_str());
      if (App_HasActiveTurn()) {
        App_StopAndListen(state_ == State::Speaking);
      }
      continue;
    }
    if (state_ != State::WaitingReply && state_ != State::Speaking) {
      continue;
    }
    if (event.kind == LinkEventKind::Text) {
      view_.AppendText(event.text);
      ui_.Show(view_);
      deadline_ = std::min(now_() + kResponseIdle, response_limit_);
    } else if (event.kind == LinkEventKind::Audio) {
      if (state_ == State::WaitingReply) {
        if (!playback::begin(generation_)) {
          App_StopAndListen(true);
          continue;
        }
        speech::reset();
        App_Enter(State::Speaking, kResponseIdle);
      }
      if (playback::write(generation_, event.audio.data(), event.audio_size, event.sequence) !=
          playback::WriteResult::Queued) {
        App_StopAndListen(true);
      } else {
        deadline_ = std::min(now_() + kResponseIdle, response_limit_);
      }
    } else if (event.kind == LinkEventKind::Done) {
      if (state_ == State::WaitingReply) {
        App_WaitForSpeech(ListenMode::FollowUp);
      } else if (!playback::finish(generation_)) {
        App_StopAndListen(true);
      } else {
        // DONE只关闭播放输入，播放线程排出滤波尾音与ALSA后才发布Drained。
        deadline_ = std::min(now_() + kDrainLimit, response_limit_);
      }
    }
  }
}

void App_ReadSpeech() {
  boompi::audio::CaptureFrame frame;
  const auto read = audio_capture::read(&frame, 20ms);
  if (read == audio_capture::ReadResult::Failed) {
    App_Fail(audio_capture::error().c_str());
    return;
  }
  if (App_CheckTimeout() || read == audio_capture::ReadResult::Timeout) {
    return;
  }
  const bool speaking =
      state_ == State::Speaking && playback::status().state == playback::State::Playing;
  const speech::Result result = speech::update(frame, speaking);
  playback::set_scale(result.playback_scale);
  if (result.decision == speech::Decision::Fault) {
    if (App_HasActiveTurn()) {
      App_StopAndListen(state_ == State::Speaking);
    } else if (state_ != State::Offline) {
      speech::reset();
      App_Enter(State::Idle);
    }
    return;
  }
  if (result.decision == speech::Decision::Wake && state_ == State::Idle) {
    App_WaitForSpeech(ListenMode::Wake);
    return;
  }

  const bool barge = result.decision == speech::Decision::Barge && speaking;
  if ((result.decision == speech::Decision::Start && state_ == State::Listening) || barge) {
    if (barge) {
      playback::cancel();
    }
    if (!App_NextGeneration() || !App_Sent(voice_net::start(generation_, barge))) {
      return;
    }
    sent_frames_ = 0;
    view_.ClearText();
    App_Enter(State::Uploading, kInputLimit);
  }

  // START → 句首原缓冲/当前帧 → END。这里只借用PCM，没有第二份AudioEvent批次。
  for (std::size_t i = 0; state_ == State::Uploading && i < result.count; ++i) {
    if (!App_Sent(voice_net::send(generation_, result.frames[i]->pcm.data()))) {
      return;
    }
    ++sent_frames_;
    if ((result.end && i + 1 == result.count) || sent_frames_ >= kMaximumInputFrames) {
      if (!App_Sent(voice_net::end(generation_))) {
        return;
      }
      speech::reset();
      response_limit_ = now_() + kResponseLimit;
      App_Enter(State::WaitingReply, kResponseIdle);
      return;
    }
  }
}

void App_ReadUserAction() {
  ui::UiAction action;
  if (!ui_.PollAction(&action)) {
    return;
  }
  if (action.kind == ui::UiActionKind::Volume) {
    view_.volume = std::min<std::uint8_t>(100, action.volume);
    playback::set_volume(view_.volume);
    ui_.Show(view_);
  } else if (action.kind == ui::UiActionKind::Interrupt) {
    if (state_ == State::Speaking || state_ == State::WaitingReply) {
      App_StopAndListen(true);
    }
  } else if (state_ == State::Idle) {
    App_WaitForSpeech(ListenMode::Wake);
  }
}
}  // namespace

bool App_Init(const boompi::config::VoiceClientConfig& config, AppReadClock clock) {
  now_ = clock;
  error_[0] = '\0';
  view_ = {};
  generation_ = sent_frames_ = 0;
  state_ = State::Offline;
  deadline_ = Clock::time_point::max();
  response_limit_ = {};
  speech::reset();
  try {
    view_.volume = ui::DeviceUi::LoadVolume();
    if (!ui_.Open()) {
      std::fprintf(stderr, "boompi: display unavailable; voice continues\n");
    }
    if (!audio_capture::open()) {
      return App_Fail(audio_capture::error().c_str());
    }
    if (!playback::open(view_.volume)) {
      return App_Fail(playback::error().c_str());
    }
    // 保持BSP原来的顺序：两路PCM都配置好之后，才开始首次读取。
    if (!audio_capture::start()) {
      return App_Fail(audio_capture::error().c_str());
    }
    if (!voice_net::open(config)) {
      return App_Fail("network startup failed");
    }
    App_Enter(State::Offline);
    return true;
  } catch (...) {
    return App_Fail("application initialization failed");
  }
}

bool App_Process() {
  try {
    App_CheckTimeout();
    App_ReceiveReply();
    if (error_[0] != '\0') {
      return false;
    }
    App_ReadSpeech();
    const auto output = playback::status();
    // 设备失败不能被随后到达的断线/取消切态掩盖；取消本身不会发布Failed。
    if (output.state == playback::State::Failed) {
      return App_Fail(playback::error().c_str());
    }
    if (state_ == State::Speaking && output.generation == generation_) {
      if (output.state == playback::State::Drained) {
        App_WaitForSpeech(ListenMode::FollowUp);
      }
    }
    App_ReadUserAction();
    return error_[0] == '\0';
  } catch (...) {
    return App_Fail("application processing failed");
  }
}

void App_Close() noexcept {
  voice_net::close();
  playback::close();
  audio_capture::close();
  speech::reset();
  ui_.Close();
}

const char* App_GetError() noexcept {
  return error_;
}
