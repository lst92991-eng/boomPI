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
  frame.pcm.fill(static_cast<std::int16_t>(id));
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
  for (int id = 1; id <= 41; ++id) {
    tick(sound(id, id <= 6));
  }
  require(state.starts == 1 && state.ends == 1 && state.samples.size() == 41,
          "START/PCM/END not delivered once");
  for (int id = 1; id <= 41; ++id) {
    require(state.samples[id - 1] == id, "application duplicated or omitted current/tail PCM");
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
  if (harness::state.output == State::Playing) {
    ++harness::state.drops;
    harness::state.output = State::Idle;
  }
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
struct DeviceUi::Impl {};
DeviceUi::~DeviceUi() noexcept = default;
std::uint8_t DeviceUi::LoadVolume(std::uint8_t fallback) noexcept {
  return fallback;
}
bool DeviceUi::Open() {
  return true;
}
void DeviceUi::Show(const UiView& view) noexcept {
  harness::state.view = view;
}
bool DeviceUi::PollAction(UiAction* action) noexcept {
  if (!harness::state.action) {
    return false;
  }
  *action = *harness::state.action;
  harness::state.action.reset();
  return true;
}
void DeviceUi::Close() noexcept {
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
      state.output_ok = stage != 1;
      state.start_ok = stage != 2;
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
    for (int id = 42; id <= 82; ++id) {
      tick(sound(id, id <= 47));
    }
    require(state.starts == 2 && state.ends == 2 && state.samples.size() == 82,
            "follow-up did not reuse the same speech path");
    open();
    question();
    network(LinkEventKind::Audio);
    for (int id = 100; id < 141; ++id) {
      tick(sound(id, id < 106));
    }
    require(state.supersede && state.drops == 1 && state.starts == 2 && state.ends == 2,
            "same spoken sentence did not replace playback");
    require(
        state.samples[41] == 100 && state.samples.back() == 140 && state.samples.size() == 82,
        "barge sentence lost its beginning or tail");
    open();
    question();
    network(LinkEventKind::Audio);
    network(LinkEventKind::Done);
    for (int id = 100; id < 105; ++id) {
      tick(sound(id, true));
    }
    state.output = playback::State::Drained;
    tick(sound(105, true));
    for (int id = 106; id < 141; ++id) {
      tick(sound(id, false));
    }
    require(state.starts == 2 && !state.supersede && state.samples.size() == 82 &&
                state.samples[41] == 100,
            "natural drain cleared a sentence that already started");
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
    for (int id = 0; id < 6; ++id) {
      tick(sound(id, true));
    }
    require(state.cancels == 2 && !state.uploading && state.samples.size() == 82,
            "backpressure skipped PCM and continued uploading");
    App_Close();
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    App_Close();
    return 1;
  }
}
