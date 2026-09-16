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
void set_state(State next) {
  constexpr ui::DeviceUiState labels[] = {ui::DeviceUiState::Idle, ui::DeviceUiState::Listening,
                                          ui::DeviceUiState::Thinking,
                                          ui::DeviceUiState::Speaking};
  state = next;
  view.state =
      voice_net::online() ? labels[static_cast<unsigned>(next)] : ui::DeviceUiState::Offline;
  ui::show(view);
}

/** @brief 开启全新的监听窗口；与保留前滚的正常尾播追问路径区分。 */
void begin_listening(Clock::duration window) {
  speech::reset();
  deadline = Clock::now() + window;
  set_state(State::Listening);
}

/** @brief 先停本地输出，再退休网络轮次；在线时重新等开口，断线时回到待唤醒。
 * retract 要求服务端撤回旧回答上下文，避免把未听完的回复当作完整对话历史。
 */
void cancel_turn(bool retract) {
  playback::cancel();
  voice_input::end_utterance();
  view.ClearText();
  if (voice_net::cancel(retract)) {
    begin_listening(3s);
  } else {
    set_state(State::Idle);
  }
}

// 主线程按回复、输入、触摸的顺序处理；各动作实现在应用入口之后。
void process_replies();
void process_voice_frame(const audio::CaptureFrame& frame);
void process_touch();
}  // namespace

bool App_Init() {
  failure[0] = '\0';
  view = {};
  stop_requested = 0;

  // 1. 准备进程。系统调用按返回值判断，不通过异常管理文件锁和信号。
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

  // 配置及网络使用会抛异常的标准库；此边界保证失败后 main 仍能调用 App_Close。
  try {
    // 2. 读配置，初始化界面和两路音频资源。
    config::VoiceClientConfig settings;
    std::string error;
    if (!config::LoadClientConfig(&settings, &error)) {
      return fail(error.c_str());
    }
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

    // 3. 两路 PCM 均已配置，才启动采集和网络，最后进入待唤醒。
    if (!voice_input::start()) {
      return fail(voice_input::error().c_str());
    }
    if (!voice_net::open(settings)) {
      return fail("network startup failed");
    }
    set_state(State::Idle);
    return true;
  } catch (const std::exception&) {
    return fail("application initialization failed");
  }
}

bool App_Process() {
  if (stop_requested) {
    return false;
  }
  // 网络投递和错误字符串仍可能抛标准异常；普通业务失败由下面的返回值分支处理。
  try {
    // 1. 接收回复，再读取一帧；网络 I/O 和实际录音在各自线程推进。
    process_replies();
    audio::CaptureFrame frame;
    const auto input = voice_input::read(frame);
    if (input == voice_input::ReadResult::Failed) {
      return fail(voice_input::error().c_str());
    }

    // 2. 尾播真正完成后才开始追问，保留前滚中用户已经说出的部分。
    const auto output = playback::status();
    if (output == playback::State::Failed) {
      return fail(playback::error().c_str());
    }
    if (state == State::Speaking && output == playback::State::Drained) {
      deadline = Clock::now() + 3s;
      set_state(State::Listening);
    }

    // 3. 断点/超时优先结束当前语句；只有连续的有效帧才能进入唤醒和语句处理。
    const bool frame_ready = input == voice_input::ReadResult::Frame;
    const bool discontinuity = frame_ready && frame.discontinuity;
    if (discontinuity) {
      debug::log.input_discontinuity();
    }
    if ((state != State::Idle && Clock::now() >= deadline) || discontinuity) {
      if (voice_net::uploading() || state == State::WaitingReply || state == State::Speaking) {
        cancel_turn(state == State::Speaking);
      } else {
        set_state(State::Idle);
      }
    } else if (frame_ready) {
      process_voice_frame(frame);
    }

    // 4. 当前语句即使取消，也继续处理触摸，避免跳过音量或按钮动作。
    process_touch();
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

namespace {
/** @brief 每轮最多消费 16 个网络事件，给输入和触摸留出执行机会。
 * AUDIO 只交付播放器；DONE 只关闭其输入，Speaking 需等 Drained 才进入追问。
 */
void process_replies() {
  voice_net::LinkEvent event;
  for (unsigned count = 0; count < 16 && voice_net::poll(event); ++count) {
    if (event.kind == LinkEventKind::Online || event.kind == LinkEventKind::Offline) {
      playback::cancel();
      view.ClearText();
      if (event.kind == LinkEventKind::Offline) {
        voice_input::end_utterance();
        debug::log.offline(event.data.c_str());
      }
      set_state(State::Idle);
      continue;
    }
    if (event.kind == LinkEventKind::Error) {
      debug::log.reply_failed(event.data.c_str());
      cancel_turn(state == State::Speaking);
      continue;
    }
    if (Clock::now() >= deadline) {
      cancel_turn(state == State::Speaking);
      continue;
    }
    deadline = std::min(Clock::now() + 30s, response_limit);
    if (event.kind == LinkEventKind::Text) {
      view.AppendText(event.data);
      ui::show(view);
    } else if (event.kind == LinkEventKind::Audio) {
      if (playback::write(event.data.data(), event.data.size()) !=
          playback::WriteResult::Queued) {
        cancel_turn(true);
      } else if (state == State::WaitingReply) {
        speech::reset();
        set_state(State::Speaking);
      }
    } else if (event.kind == LinkEventKind::Done) {
      if (state == State::WaitingReply) {
        // 本轮没有音频，无声卡尾音需要等待，可直接进入追问。
        begin_listening(3s);
      } else {
        playback::finish();
        deadline = std::min(Clock::now() + 3s, response_limit);
      }
    }
  }
}

/** @brief 连续输入帧的业务处理：待唤醒 → 语句确认 → START → PCM → END。
 * 只在主线程消费传入帧，不复制 PCM、不创建任务；断点和超时由 App_Process 先处理。
 * 发送失败只取消本轮并返回，应用仍可处理触摸并等待重连。
 */
void process_voice_frame(const audio::CaptureFrame& frame) {
  if (!voice_net::online()) {
    return;
  }
  if (state == State::Idle) {
    if (frame.wake) {
      begin_listening(6s);
    }
    return;
  }
  if (state == State::WaitingReply) {
    return;
  }

  // Listening 和 Speaking 共用语句处理；只有播放中确认的新句才替换旧回答。
  const bool replacing = state == State::Speaking;
  const auto utterance = speech::update(frame, replacing && view.volume != 0);
  if (utterance.start && replacing) {
    playback::cancel();  // 确认后直接取消，不能先解除 hold 再给旧 PCM 一次输出机会。
  } else {
    playback::hold(utterance.hold_playback);
  }
  if (utterance.start) {
    if (voice_net::start(replacing) != SendResult::Ok) {
      cancel_turn(false);
      return;
    }
    view.ClearText();
    deadline = Clock::now() + 60s;
    set_state(State::Listening);
  }
  if (!voice_net::uploading()) {
    return;
  }

  // START 的这一批包含前滚和当前帧；后续每次只有实时帧，句尾帧也先发送再 END。
  for (std::size_t i = 0; i < utterance.count; ++i) {
    if (voice_net::send(*utterance.frames[i]) != SendResult::Ok) {
      cancel_turn(false);
      return;
    }
  }
  if (!utterance.end) {
    return;
  }
  if (voice_net::end() != SendResult::Ok) {
    cancel_turn(false);
    return;
  }
  voice_input::end_utterance();
  deadline = Clock::now() + 30s;
  response_limit = Clock::now() + 300s;
  set_state(State::WaitingReply);
}

/** @brief 消费一个触摸意图；页面只发出动作，是否执行由这里的实时业务状态决定。 */
void process_touch() {
  ui::UiAction action;
  if (!ui::poll_action(action)) {
    return;
  }
  if (action.kind == ui::UiActionKind::Volume) {
    view.volume = std::min<std::uint8_t>(100, action.volume);
    playback::set_volume(view.volume);
    ui::show(view);
  } else if (action.kind == ui::UiActionKind::Interrupt) {
    if (state == State::Speaking || state == State::WaitingReply) {
      cancel_turn(true);
    }
  } else if (state == State::Idle && voice_net::online()) {
    begin_listening(6s);
  }
}
}  // namespace
