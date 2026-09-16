#include "boompi/application/voice_client.h"

#include <algorithm>
#include <chrono>
#include <cstdio>

#include "boompi/audio/playback.h"
#include "boompi/audio/speech.h"
#include "boompi/audio/voice_input.h"
#include "boompi/network/voice_net.h"
#include "boompi/ui/device_ui.h"

namespace {
using namespace boompi;
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
using voice_net::LinkEventKind;
using voice_net::SendResult;
enum class State { Idle, Listening, WaitingReply, Speaking };
State state{State::Idle};
ui::UiView view;
Clock::time_point deadline{}, response_limit{};
char failure[192]{};

bool fail(const char* reason) {
  std::snprintf(failure, sizeof(failure), "%s", reason);
  view.state = ui::DeviceUiState::Error;
  ui::show(view);
  return false;
}
void show(State next) {
  constexpr ui::DeviceUiState labels[] = {ui::DeviceUiState::Idle, ui::DeviceUiState::Listening,
                                          ui::DeviceUiState::Thinking,
                                          ui::DeviceUiState::Speaking};
  state = next;
  view.state =
      voice_net::online() ? labels[static_cast<unsigned>(next)] : ui::DeviceUiState::Offline;
  ui::show(view);
}
void listen(Clock::duration window) {
  speech::reset();
  deadline = Clock::now() + window;
  show(State::Listening);
}
void cancel(bool retract) {
  playback::cancel();
  voice_input::end_utterance();
  view.ClearText();
  if (voice_net::cancel(retract)) {
    listen(3s);
  } else {
    show(State::Idle);
  }
}
bool sent(SendResult result) {
  if (result == SendResult::Ok) {
    return true;
  }
  cancel(false);
  return false;
}

void receive_reply() {
  voice_net::LinkEvent event;
  for (unsigned count = 0; count < 16 && voice_net::poll(event); ++count) {
    if (event.kind == LinkEventKind::Online || event.kind == LinkEventKind::Offline) {
      playback::cancel();
      view.ClearText();
      if (event.kind == LinkEventKind::Offline) {
        voice_input::end_utterance();
        std::fprintf(stderr, "boompi: offline; stage=%s\n", event.data.c_str());
      }
      show(State::Idle);
      continue;
    }
    if (event.kind == LinkEventKind::Error) {
      std::fprintf(stderr, "boompi: reply failed; code=%s\n", event.data.c_str());
      cancel(state == State::Speaking);
      continue;
    }
    if (Clock::now() >= deadline) {
      cancel(state == State::Speaking);
      continue;
    }
    deadline = std::min(Clock::now() + 30s, response_limit);
    if (event.kind == LinkEventKind::Text) {
      view.AppendText(event.data);
      ui::show(view);
    } else if (event.kind == LinkEventKind::Audio) {
      if (playback::write(event.data.data(), event.data.size()) !=
          playback::WriteResult::Queued) {
        cancel(true);
      } else if (state == State::WaitingReply) {
        speech::reset();
        show(State::Speaking);
      }
    } else if (event.kind == LinkEventKind::Done) {
      if (state == State::WaitingReply) {
        listen(3s);
      } else {
        playback::finish();
        deadline = std::min(Clock::now() + 3s, response_limit);
      }
    }
  }
}
}  // namespace

bool App_Init(const boompi::config::VoiceClientConfig& config) {
  failure[0] = '\0';
  view = {};
  try {
    view.volume = ui::load_volume();
    if (!ui::open()) {
      std::fprintf(stderr, "boompi: display unavailable; voice continues\n");
    }
    if (!voice_input::open()) {
      return fail(voice_input::error().c_str());
    }
    if (!playback::open(view.volume)) {
      return fail(playback::error().c_str());
    }
    // 两路PCM都已配置后，才开始采集；建链在网络线程内进行。
    if (!voice_input::start()) {
      return fail(voice_input::error().c_str());
    }
    if (!voice_net::open(config)) {
      return fail("network startup failed");
    }
    show(State::Idle);
    return true;
  } catch (...) {
    return fail("application initialization failed");
  }
}

bool App_Process() {
  try {
    receive_reply();
    audio::CaptureFrame frame;
    const auto input = voice_input::read(frame);
    if (input == voice_input::ReadResult::Failed) {
      return fail(voice_input::error().c_str());
    }
    const auto output = playback::status();
    if (output == playback::State::Failed) {
      return fail(playback::error().c_str());
    }
    if (state == State::Speaking && output == playback::State::Drained) {
      // 尾播完成才打开追问窗口；不清前滚，用户可能已经开始了下一句话。
      deadline = Clock::now() + 3s;
      show(State::Listening);
    }
    // 超时或缺帧不能继续提交残缺语句；停止后才重新等待开口。
    if (input == voice_input::ReadResult::Frame && frame.discontinuity) {
      std::fprintf(stderr, "boompi: input discontinuity; current input canceled\n");
    }
    if ((state != State::Idle && Clock::now() >= deadline) ||
        (input == voice_input::ReadResult::Frame && frame.discontinuity)) {
      if (voice_net::uploading() || state == State::WaitingReply || state == State::Speaking) {
        cancel(state == State::Speaking);
      } else {
        show(State::Idle);
      }
    } else if (input == voice_input::ReadResult::Frame && voice_net::online()) {
      if (state == State::Idle && frame.wake) {
        listen(6s);
      } else if (state == State::Listening || state == State::Speaking) {
        const bool replacing = state == State::Speaking;
        const auto speech_frame = speech::update(frame, replacing && view.volume != 0);
        // 确认后直接取消，不能先解除hold给旧PCM一次复活的机会。
        if (speech_frame.start && replacing) {
          playback::cancel();
        } else {
          playback::hold(speech_frame.hold_playback);
        }
        if (speech_frame.start) {
          if (!sent(voice_net::start(replacing))) {
            return true;
          }
          view.ClearText();
          deadline = Clock::now() + 60s;
          show(State::Listening);
        }
        // 一次START之后依次交付前滚、实时PCM，最后才END。
        if (voice_net::uploading()) {
          for (std::size_t i = 0; i < speech_frame.count; ++i) {
            if (!sent(voice_net::send(*speech_frame.frames[i]))) {
              return true;
            }
          }
          if (speech_frame.end) {
            if (!sent(voice_net::end())) {
              return true;
            }
            voice_input::end_utterance();
            deadline = Clock::now() + 30s;
            response_limit = Clock::now() + 300s;
            show(State::WaitingReply);
          }
        }
      }
    }
    ui::UiAction action;
    if (ui::poll_action(action)) {
      if (action.kind == ui::UiActionKind::Volume) {
        view.volume = std::min<std::uint8_t>(100, action.volume);
        playback::set_volume(view.volume);
        ui::show(view);
      } else if (action.kind == ui::UiActionKind::Interrupt) {
        if (state == State::Speaking || state == State::WaitingReply) {
          cancel(true);
        }
      } else if (state == State::Idle && voice_net::online()) {
        listen(6s);
      }
    }
    return true;
  } catch (...) {
    return fail("application processing failed");
  }
}

void App_Close() noexcept {
  voice_net::close();
  playback::close();
  voice_input::close();
  ui::close();
}
const char* App_GetError() noexcept {
  return failure;
}
