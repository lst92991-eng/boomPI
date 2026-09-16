/** @file voice_client.cpp
 * @brief 小智应用主流程：初始化 → 处理回复/输入/触摸 → 停止并回收。
 *
 * 本文件由主线程执行，独占对话四态、字幕和等待期限。
 * 输入任务只交付 PCM/检测结果，网络拥有轮次与序号，播放任务报告实际尾播完成。
 */
#include "boompi/application/voice_client.h"

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <exception>

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
/** @brief Listening 包含等开口和上传；是否已经 START 直接查询网络，不另存布尔副本。 */
enum class State { Idle, Listening, WaitingReply, Speaking };
State state{State::Idle};
ui::UiView view;
// deadline 为当前阶段期限；回复每到一包可续期，但不能超过 response_limit 总时限。
Clock::time_point deadline{}, response_limit{};
char failure[192]{};
int instance_lock{-1};
volatile std::sig_atomic_t stop_requested{0};

void request_stop(int) {
  // 信号回调只通知退出；线程和硬件留给正常流程关闭。
  stop_requested = 1;
}

/** @brief 记录不可继续的应用故障；返回 false 让 main 进入统一收尾。 */
bool fail(const char* reason) {
  std::snprintf(failure, sizeof(failure), "%s", reason);
  view.state = ui::DeviceUiState::Error;
  ui::show(view);
  return false;
}

/** @brief 唯一的业务状态写入口，同时把状态映射为 UI 快照。 */
void show(State next) {
  constexpr ui::DeviceUiState labels[] = {ui::DeviceUiState::Idle, ui::DeviceUiState::Listening,
                                          ui::DeviceUiState::Thinking,
                                          ui::DeviceUiState::Speaking};
  state = next;
  view.state =
      voice_net::online() ? labels[static_cast<unsigned>(next)] : ui::DeviceUiState::Offline;
  ui::show(view);
}

/** @brief 开启全新的监听窗口；与保留前滚的正常尾播追问路径区分。 */
void listen(Clock::duration window) {
  speech::reset();
  deadline = Clock::now() + window;
  show(State::Listening);
}

/** @brief 先停本地输出，再退休网络轮次；在线时重新等开口，断线时回到待唤醒。
 * retract 要求服务端撤回旧回答上下文，避免把未听完的回复当作完整对话历史。
 */
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

/** @brief 每轮最多消费 16 个网络事件，给输入和触摸留出执行机会。
 * AUDIO 只交付播放器；DONE 只关闭其输入，Speaking 需等 Drained 才进入追问。
 */
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
        // 本轮没有音频，无声卡尾音需要等待，可直接进入追问。
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
  // 配置/网络仍使用会抛异常的 C++ 标准库；在应用边界转为失败，main 仍可 Close。
  // 声卡和算法的正常失败均在下面直接检查返回值，不借异常跳转业务流程。
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
  } catch (const std::exception&) {
    return fail("application initialization failed");
  }
}

bool App_Process() {
  if (stop_requested) {
    return false;
  }
  // 网络投递及错误原因复制仍可能抛标准库异常；只在应用边界收住，保证 main 能收尾。
  try {
    // 1. 接收回答，再取一帧采集结果；网络连接与实际录音都由各自线程推进。
    receive_reply();
    audio::CaptureFrame frame;
    const auto input = voice_input::read(frame);
    if (input == voice_input::ReadResult::Failed) {
      return fail(voice_input::error().c_str());
    }
    // 2. 先检查播放是否故障/真正结束，随后再用当前输入判断追问或插话。
    const auto output = playback::status();
    if (output == playback::State::Failed) {
      return fail(playback::error().c_str());
    }
    if (state == State::Speaking && output == playback::State::Drained) {
      // 尾播完成才打开追问窗口；不清前滚，用户可能已经开始了下一句话。
      deadline = Clock::now() + 3s;
      show(State::Listening);
    }
    // 3. 超时或缺帧先终止当前语句；不能把断点前后的 PCM 拼到一起上传。
    const bool frame_ready = input == voice_input::ReadResult::Frame;
    const bool discontinuity = frame_ready && frame.discontinuity;
    if (discontinuity) {
      debug::log.input_discontinuity();
    }
    if ((state != State::Idle && Clock::now() >= deadline) || discontinuity) {
      if (voice_net::uploading() || state == State::WaitingReply || state == State::Speaking) {
        cancel(state == State::Speaking);
      } else {
        show(State::Idle);
      }
    } else if (frame_ready && voice_net::online()) {
      // 4. 同一帧只进入当前状态的处理；等待回复期间不把环境声当作新问题。
      switch (state) {
        case State::Idle:
          if (frame.wake) {
            listen(6s);
          }
          break;
        case State::WaitingReply:
          break;
        case State::Listening:
        case State::Speaking: {
          const bool replacing = state == State::Speaking;
          const auto speech_frame = speech::update(frame, replacing && view.volume != 0);
          // 确认后直接取消，不能先解除hold给旧PCM一次复活的机会。
          if (speech_frame.start && replacing) {
            playback::cancel();
          } else {
            playback::hold(speech_frame.hold_playback);
          }
          if (speech_frame.start) {
            if (voice_net::start(replacing) != SendResult::Ok) {
              cancel(false);
              // 只放弃当前语句，主循环继续，以便断线恢复后再次使用。
              return true;
            }
            view.ClearText();
            deadline = Clock::now() + 60s;
            show(State::Listening);
          }
          // 一次START之后依次交付前滚、实时PCM，最后才END。
          if (voice_net::uploading()) {
            for (std::size_t i = 0; i < speech_frame.count; ++i) {
              if (voice_net::send(*speech_frame.frames[i]) != SendResult::Ok) {
                cancel(false);
                return true;
              }
            }
            if (speech_frame.end) {
              if (voice_net::end() != SendResult::Ok) {
                cancel(false);
                return true;
              }
              voice_input::end_utterance();
              deadline = Clock::now() + 30s;
              response_limit = Clock::now() + 300s;
              show(State::WaitingReply);
            }
          }
          break;
        }
      }
    }
    // 5. 触摸只产生意图；开始、停止和音量由同一个主线程执行，避免与语音状态竞争。
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
  } catch (const std::exception&) {
    return fail("application processing failed");
  }
}

int App_Close() noexcept {
  // 先停止新的网络交付，再停止音频任务；各 close 在 join 后才释放自身资源。
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
