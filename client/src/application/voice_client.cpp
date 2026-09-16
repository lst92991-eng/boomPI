#include "boompi/application/voice_client.h"

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>

#include "boompi/audio/playback.h"
#include "boompi/audio/speech.h"
#include "boompi/audio/voice_input.h"
#include "boompi/config/voice_client_config.h"
#include "boompi/debug.h"
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
int instance_lock{-1};
volatile std::sig_atomic_t stop_requested{0};

void request_stop(int) {
  // 信号回调只通知退出；线程和硬件留给正常流程关闭。
  stop_requested = 1;
}

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
        debug::log.offline(event.data.c_str());
      }
      show(State::Idle);
      continue;
    }
    if (event.kind == LinkEventKind::Error) {
      debug::log.reply_failed(event.data.c_str());
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

bool App_Init() {
  failure[0] = '\0';
  view = {};
  stop_requested = 0;
  try {
    // 1. 准备当前进程，防止重复打开声卡和屏幕。
    instance_lock = ::open("/run/boompi-client.lock", O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    if (instance_lock < 0) {
      return fail("cannot open instance lock");
    }
    if (flock(instance_lock, LOCK_EX | LOCK_NB) < 0) {
      return fail("client is already running or instance lock failed");
    }
    std::signal(SIGINT, request_stop);
    std::signal(SIGTERM, request_stop);
    std::signal(SIGPIPE, SIG_IGN);

    // 2. 读取本机配置，所有模块使用同一份启动参数。
    config::VoiceClientConfig settings;
    std::string error;
    if (!config::LoadClientConfig(&settings, &error)) {
      return fail(error.c_str());
    }

    // 3. 初始化界面、输入处理和播放。
    view.volume = ui::load_volume();
    if (!ui::open()) {
      debug::log.display_unavailable();
    }
    if (!voice_input::open()) {
      return fail(voice_input::error().c_str());
    }
    if (!playback::open(view.volume)) {
      return fail(playback::error().c_str());
    }
    // 4. 两路PCM配置完成后启动采集，再启动网络线程。
    if (!voice_input::start()) {
      return fail(voice_input::error().c_str());
    }
    if (!voice_net::open(settings)) {
      return fail("network startup failed");
    }
    show(State::Idle);
    return true;
  } catch (...) {
    return fail("application initialization failed");
  }
}

bool App_Process() {
  if (stop_requested) {
    return false;
  }
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
      debug::log.input_discontinuity();
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

int App_Close() noexcept {
  voice_net::close();
  playback::close();
  voice_input::close();
  ui::close();
  if (instance_lock >= 0) {
    ::close(instance_lock);
    instance_lock = -1;
  }
  if (failure[0]) {
    debug::log.failure(failure);
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
