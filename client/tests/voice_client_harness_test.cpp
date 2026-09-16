// 执行真实应用和speech；只检查模块交付，真实时间和声学体验留给上板。
#include <deque>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <vector>

#include "boompi/application/voice_client.h"
#include "boompi/audio/playback.h"
#include "boompi/audio/speech.h"
#include "boompi/audio/voice_input.h"
#include "boompi/network/voice_net.h"
#include "boompi/ui/device_ui.h"

namespace harness {
using namespace boompi;
struct State {
  std::optional<audio::CaptureFrame> frame;
  std::optional<ui::UiAction> action;
  std::deque<voice_net::LinkEvent> events;
  std::vector<int> samples;
  ui::UiView view;
  playback::State output{playback::State::Idle};
  bool online{false}, uploading{false}, supersede{false}, fail_send{false};
  bool hold_requested{false};
  bool input_ok{true}, start_ok{true}, output_ok{true}, network_ok{true};
  int starts{0}, ends{0}, cancels{0}, drops{0}, finishes{0}, resets{0}, closes{0};
} state;
void require(bool pass, const char* reason) {
  if (!pass) {
    throw std::runtime_error(reason);
  }
}
audio::CaptureFrame sound(int id, bool voice) {
  audio::CaptureFrame frame;
  for (std::size_t i = 0; i < frame.pcm.size(); ++i) {
    frame.pcm[i] = static_cast<std::int16_t>(voice ? (i % 2 ? -4096 : 4096) : 0);
  }
  frame.pcm[0] = static_cast<std::int16_t>(id);
  frame.vad_now = voice;
  return frame;
}
void tick(std::optional<audio::CaptureFrame> frame = {}) {
  state.frame = frame;
  require(App_Process(), App_GetError());
}
void network(voice_net::LinkEventKind kind) {
  voice_net::LinkEvent event;
  event.kind = kind;
  if (kind == voice_net::LinkEventKind::Audio) {
    event.data = {1, 0};
  }
  state.events.push_back(std::move(event));
  tick();
}
void open() {
  App_Close();
  state = {};
  require(App_Init({}), "application init");
  network(voice_net::LinkEventKind::Online);
}
void question() {
  auto wake = sound(0, false);
  wake.wake = true;
  tick(wake);
  for (int id = 1; id <= 50; ++id) {
    tick(sound(id, id <= 15));
  }
  require(state.starts == 1 && state.ends == 1 && state.samples.size() == 50,
          "START/PCM/END not delivered once");
  for (int id = 1; id <= 50; ++id) {
    require(state.samples[id - 1] == id, "application duplicated or omitted current/tail PCM");
  }
}
audio::CaptureFrame barge_sound(int id, bool reference, bool held, bool voice = true) {
  auto frame = sound(id, voice);
  frame.reference_active = reference;
  frame.playback_held = held;
  return frame;
}
void reject_probes() {
  // 缺参考不放行；软件已请求hold但声卡尚未写静音时，也不能确认。
  for (int scenario = 0; scenario < 4; ++scenario) {
    open();
    question();
    network(voice_net::LinkEventKind::Audio);
    for (int id = 0; id < 40; ++id) {
      auto frame = barge_sound(id, scenario != 0 && id < 6, scenario >= 2 && id >= 6);
      if (scenario == 3) {
        frame.reference_active = true;
      }
      if (scenario == 2 && id >= 6) {
        // 回声静音后消失，但VAD保持为真；直流偏置也不能伪装成近讲。
        frame.pcm.fill(20000);
      }
      tick(frame);
    }
    require(state.starts == 1 && state.drops == 0 && state.cancels == 0 &&
                state.samples.size() == 50 && !state.hold_requested,
            "echo/missing reference/unapplied hold created a new turn");
    require(state.view.state == ui::DeviceUiState::Speaking, "rejected probe lost old reply");
  }
  open();
  question();
  network(voice_net::LinkEventKind::Audio);
  for (int id = 0; id < 6; ++id) {
    tick(barge_sound(id, true, false));
  }
  require(state.hold_requested, "candidate should hold before any START");
  auto gap = sound(0, false);
  gap.discontinuity = true;
  tick(gap);
  require(!state.hold_requested && state.cancels == 1 && state.starts == 1,
          "discontinuity left a probe or partial input alive");
}
void confirmed_barge(bool natural_end, int reference_delay = 0) {
  open();
  question();
  network(voice_net::LinkEventKind::Audio);
  if (natural_end) {
    network(voice_net::LinkEventKind::Done);
  }
  const int confirm_at = 116 + reference_delay;
  for (int id = 100; id <= confirm_at; ++id) {
    if (natural_end && id == 106) {
      state.output = playback::State::Drained;
    }
    tick(barge_sound(id, id < 106 + reference_delay, id >= 106 && !natural_end));
    if (id < confirm_at) {
      require(state.starts == 1 && state.drops == 0, "candidate retired old reply early");
    }
    if (id == 105) {
      require(state.hold_requested, "early barge hidden by startup warmup");
    }
  }
  require(state.starts == 2 && state.supersede == !natural_end &&
              state.drops == (natural_end ? 0 : 1) && !state.hold_requested,
          "confirmed input did not take ownership exactly once");
  for (int id = confirm_at + 1; id <= confirm_at + 35; ++id) {
    tick(sound(id, false));
  }
  require(state.ends == 2 && state.samples.size() == static_cast<std::size_t>(102 + reference_delay),
          "confirmed input missing END or PCM");
  for (int id = 100; id <= confirm_at + 35; ++id) {
    require(state.samples[50 + id - 100] == id, "probe pre-roll/current/tail skipped or repeated");
  }
}
}  // namespace harness
namespace boompi::voice_input {
bool open() {
  return harness::state.input_ok;
}
bool start() {
  return harness::state.start_ok;
}
ReadResult read(audio::CaptureFrame& frame) {
  if (!harness::state.frame) {
    return ReadResult::Timeout;
  }
  frame = *harness::state.frame;
  harness::state.frame.reset();
  return ReadResult::Frame;
}
void end_utterance() noexcept {
  ++harness::state.resets;
}
std::string error() {
  return "injected capture failure";
}
void close() {
  ++harness::state.closes;
}
}  // namespace boompi::voice_input
namespace boompi::playback {
bool open(std::uint8_t) {
  return harness::state.output_ok;
}
WriteResult write(const void*, std::size_t) {
  harness::state.output = State::Playing;
  return WriteResult::Queued;
}
void finish() {
  ++harness::state.finishes;
}
void cancel() {
  harness::state.hold_requested = false;
  if (harness::state.output == State::Playing) {
    ++harness::state.drops;
    harness::state.output = State::Idle;
  }
}
void hold(bool enabled) {
  harness::state.hold_requested = enabled && harness::state.output == State::Playing;
}
void set_volume(std::uint8_t) {}
State status() {
  return harness::state.output;
}
std::string error() {
  return "injected playback failure";
}
void close() {
  ++harness::state.closes;
}
}  // namespace boompi::playback
namespace boompi::voice_net {
bool open(const config::VoiceClientConfig&) {
  return harness::state.network_ok;
}
bool online() {
  return harness::state.online;
}
bool uploading() {
  return harness::state.uploading;
}
bool poll(LinkEvent& event) {
  auto& s = harness::state;
  if (s.events.empty()) {
    return false;
  }
  event = std::move(s.events.front());
  s.events.pop_front();
  if (event.kind == LinkEventKind::Online || event.kind == LinkEventKind::Offline) {
    s.online = event.kind == LinkEventKind::Online;
    s.uploading = false;
  }
  return true;
}
SendResult start(bool supersede) {
  harness::require(!harness::state.hold_requested, "START before releasing/canceling probe");
  ++harness::state.starts;
  harness::state.uploading = true;
  harness::state.supersede = supersede;
  return SendResult::Ok;
}
SendResult send(const audio::VoiceFrame16k& pcm) {
  if (harness::state.fail_send) {
    return SendResult::Backpressure;
  }
  harness::state.samples.push_back(pcm[0]);
  return SendResult::Ok;
}
SendResult end() {
  ++harness::state.ends;
  harness::state.uploading = false;
  return SendResult::Ok;
}
bool cancel(bool) {
  ++harness::state.cancels;
  harness::state.uploading = false;
  return harness::state.online;
}
void close() noexcept {
  ++harness::state.closes;
}
}  // namespace boompi::voice_net
namespace boompi::ui {
std::uint8_t load_volume(std::uint8_t fallback) noexcept {
  return fallback;
}
bool open() {
  return true;
}
void show(const UiView& view) noexcept {
  harness::state.view = view;
}
bool poll_action(UiAction& action) noexcept {
  if (!harness::state.action) {
    return false;
  }
  action = *harness::state.action;
  harness::state.action.reset();
  return true;
}
void close() noexcept {
  ++harness::state.closes;
}
}  // namespace boompi::ui
int main() {
  using namespace harness;
  using namespace boompi;
  using voice_net::LinkEventKind;
  try {
    for (int stage = 0; stage < 4; ++stage) {
      state = {};
      state.input_ok = stage != 0;
      state.start_ok = stage != 1;
      state.output_ok = stage != 2;
      state.network_ok = stage != 3;
      require(!App_Init({}), "initialization failure ignored");
      App_Close();
      require(state.closes == 4, "partial initialization not released");
    }
    open();
    question();
    network(LinkEventKind::Audio);
    network(LinkEventKind::Done);
    require(state.finishes == 1 && state.view.state == ui::DeviceUiState::Speaking,
            "DONE started follow-up before physical drain");
    state.output = playback::State::Drained;
    tick();
    require(state.view.state == ui::DeviceUiState::Listening, "drain did not start follow-up");
    for (int id = 51; id <= 100; ++id) {
      tick(sound(id, id <= 65));
    }
    require(state.starts == 2 && state.ends == 2 && state.samples.size() == 100,
            "follow-up did not reuse the same speech path");
    reject_probes();
    confirmed_barge(false);
    confirmed_barge(false, 8);  // 最晚允许确认恰好保留25帧，不扩大网络突发。
    confirmed_barge(true);
    state.action = ui::UiAction{ui::UiActionKind::Interrupt, 60};
    tick();
    require(state.cancels == 1, "END incorrectly disabled cancel");
    network(LinkEventKind::Offline);
    network(LinkEventKind::Online);
    require(state.starts == 2 && state.view.state == ui::DeviceUiState::Idle,
            "reconnect resubmitted old speech");
    auto wake = sound(0, false);
    wake.wake = true;
    tick(wake);
    state.fail_send = true;
    for (int id = 0; id < 15; ++id) {
      tick(sound(id, true));
    }
    require(state.cancels == 2 && !state.uploading && state.samples.size() == 102,
            "backpressure skipped PCM and continued uploading");
    App_Close();
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    App_Close();
    return 1;
  }
}
