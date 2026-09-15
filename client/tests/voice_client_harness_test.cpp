// 运行真实App_Process与speech；只替换设备线程、网络I/O、UI和时钟。
#include <algorithm>
#include <csignal>
#include <cstdlib>
#include <deque>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <vector>

#include "boompi/application/voice_client.h"
#include "boompi/audio/playback.h"
#include "boompi/audio/speech.h"
#include "boompi/audio/voice_input.h"
#include "boompi/network/voice_net.h"
#include "boompi/ui/device_ui.h"
#include "vad.h"
#include "wake.h"

#ifndef _WIN32
#define main BoompiClientMain
#include "../apps/boompi_client/main.cpp"
#undef main
#endif

namespace harness {
using boompi::audio::CaptureFrame;
using boompi::ui::DeviceUiState;
using boompi::ui::UiAction;
using boompi::ui::UiActionKind;
using boompi::voice_net::LinkEvent;
using boompi::voice_net::LinkEventKind;
using boompi::voice_net::SendResult;

struct Step {
  std::optional<CaptureFrame> capture;
  std::vector<LinkEvent> network;
  std::optional<UiAction> user;
  std::optional<boompi::playback::Status> output;
  unsigned elapsed_ms{20};
};
struct Sent {
  std::string kind;
  std::uint32_t generation;
  std::int16_t sample{0};
  bool retract{false};
};
struct State {
  std::vector<Step> steps;
  std::size_t next{0};
  bool stopped{false};
  std::uint64_t time_ms{0};
  std::deque<LinkEvent> inbound;
  std::optional<UiAction> action;
  std::vector<Sent> sent;
  std::vector<SendResult> results;
  std::size_t send_calls{0};
  std::vector<boompi::ui::UiView> views;
  std::vector<std::uint32_t> played, finished;
  std::vector<float> scales;
  boompi::playback::Status output;
  unsigned audio_opens{0}, link_opens{0}, playback_opens{0};
  unsigned audio_closes{0}, link_closes{0}, playback_closes{0}, ui_closes{0}, resets{0};
  bool healthy{true}, play_ok{true}, stop_ok{true}, listener_ok{true};
  bool audio_open_ok{true}, link_open_ok{true}, playback_open_ok{true}, ui_open_ok{true};
  bool capture_start_ok{true};
  bool throw_on_audio_open{false}, throw_on_audio_process{false};
  std::string saved_ssid, saved_password;
  CaptureFrame current;
  bool connected{false}, input_open{false};
  std::uint32_t input_frames{0};
} state;

auto Now() {
  return std::chrono::steady_clock::time_point(std::chrono::milliseconds(state.time_ms));
}
CaptureFrame Sound(int sample, bool start = false, bool end = false) {
  CaptureFrame frame;
  for (std::size_t i = 0; i < frame.pcm.size(); ++i) {
    frame.pcm[i] = i % 2 ? -4096 : 4096;
  }
  frame.pcm[0] = static_cast<std::int16_t>(sample);
  frame.input_dbfs = -20;
  frame.vad_now = frame.near_voice = !end;
  frame.voice_dbfs = -10;
  frame.vad_started = start;
  frame.vad_ended = end;
  return frame;
}
Step A(CaptureFrame frame) {
  Step step;
  step.capture = frame;
  return step;
}
LinkEvent Net(LinkEventKind kind, unsigned gen = 0) {
  LinkEvent event;
  event.kind = kind;
  event.generation = gen;
  return event;
}
Step N(LinkEvent event) {
  Step step;
  step.network.push_back(event);
  return step;
}
Step Delay(unsigned ms) {
  Step step;
  step.elapsed_ms = ms;
  return step;
}
Step User(UiActionKind kind, std::uint8_t volume = 60) {
  Step step;
  step.user = UiAction{kind, volume};
  return step;
}
Step Drained(unsigned gen) {
  Step step;
  step.output = boompi::playback::Status{gen, boompi::playback::State::Drained};
  return step;
}
LinkEvent Reply(unsigned gen = 1) {
  auto event = Net(LinkEventKind::Audio, gen);
  event.audio_size = 2;  // 一样本回复，测试实际末包不依赖START/END flags。
  return event;
}
std::vector<Step> Question() {
  auto wake = Sound(0);
  wake.wake = true;
  std::vector<Step> steps{N(Net(LinkEventKind::Online)), A(wake)};
  for (int i = 0; i < 6; ++i) {
    steps.push_back(A(Sound(11 + i)));
  }
  for (int i = 0; i < 35; ++i) {
    steps.push_back(A(Sound(0, false, true)));
  }
  return steps;
}
std::vector<SendResult> EndBackpressure() {
  std::vector<SendResult> values(43, SendResult::Ok);
  values.back() = SendResult::Backpressure;
  return values;
}
std::size_t Count(const char* kind) {
  std::size_t count = 0;
  for (const auto& sent : state.sent) {
    count += sent.kind == kind;
  }
  return count;
}
SendResult Send(Sent sent) {
  state.sent.push_back(std::move(sent));
  const auto index = state.send_calls++;
  const auto result = index < state.results.size() ? state.results[index] : SendResult::Ok;
  if (result == SendResult::Disconnected) {
    state.connected = false;
  }
  return result;
}
bool Run(std::vector<Step> steps, std::vector<SendResult> results = {}, bool play_ok = true,
         bool stop_ok = true) {
  state = {};
  state.steps = std::move(steps);
  state.results = std::move(results);
  state.play_ok = play_ok;
  state.stop_ok = stop_ok;
  boompi::config::VoiceClientConfig config;
  config.device_id = "00112233-4455-4677-8899-aabbccddeeff";
  bool ok = App_Init(config, &Now);
  while (!state.stopped && ok) {
    ok = App_Process();
  }
  App_Close();
  return ok;
}
}  // namespace harness

namespace boompi::voice_input {
bool start() {
  return harness::state.capture_start_ok;
}
bool open() {
  ++harness::state.audio_opens;
  if (harness::state.throw_on_audio_open) {
    throw std::runtime_error("scripted startup exception");
  }
  return harness::state.audio_open_ok;
}
ReadResult read(audio::CaptureFrame* frame, std::chrono::milliseconds) {
  auto& s = harness::state;
  if (s.throw_on_audio_process) {
    throw std::runtime_error("scripted processing exception");
  }
  if (!s.healthy) {
    return ReadResult::Failed;
  }
  if (s.next == s.steps.size()) {
    s.stopped = true;
    return ReadResult::Timeout;
  }
  auto step = s.steps[s.next++];
  s.time_ms += step.elapsed_ms;
  for (auto& event : step.network) {
    s.inbound.push_back(std::move(event));
  }
  s.action = step.user;
  if (step.output) {
    s.output = *step.output;
  }
  if (!step.capture) {
    return ReadResult::Timeout;
  }
  *frame = *step.capture;
  frame->output = boompi::playback::observe();
  s.current = *frame;
  return ReadResult::Frame;
}
std::string error() {
  return "scripted audio error";
}
void close() {
  ++harness::state.audio_closes;
}
}  // namespace boompi::voice_input

namespace boompi::wake {
bool open() noexcept {
  return true;
}
bool reset() noexcept {
  return true;
}
bool detect(const audio::VoiceFrame16k&, bool* detected) noexcept {
  *detected = harness::state.current.wake;
  return true;
}
const char* error() noexcept {
  return "scripted wake error";
}
void close() noexcept {}
}  // namespace boompi::wake
namespace boompi::vad {
bool open() noexcept {
  return true;
}
bool reset() noexcept {
  ++harness::state.resets;
  return harness::state.listener_ok;
}
int process(const audio::VoiceFrame16k&) noexcept {
  return harness::state.current.vad_now ? 1 : 0;
}
const char* error() noexcept {
  return "scripted VAD error";
}
void close() noexcept {}
}  // namespace boompi::vad
namespace boompi::playback {
bool open(std::uint8_t) {
  ++harness::state.playback_opens;
  return harness::state.playback_open_ok;
}
bool begin(std::uint32_t gen) {
  harness::state.output = {gen, State::Playing};
  return true;
}
WriteResult write(std::uint32_t gen, const std::uint8_t*, std::size_t) {
  harness::state.played.push_back(gen);
  return harness::state.play_ok ? WriteResult::Queued : WriteResult::Full;
}
bool finish(std::uint32_t gen) {
  harness::state.finished.push_back(gen);
  return true;
}
void cancel() {
  if (harness::state.output.state == State::Playing) {
    harness::state.output = {};
  }
}
void set_volume(std::uint8_t) {}
void set_scale(float scale) {
  harness::state.scales.push_back(scale);
}
Observation observe() {
  return {harness::state.output.state == State::Playing, true, End::None};
}
Status status() {
  return harness::state.output;
}
std::string error() {
  return "scripted playback error";
}
void close() {
  ++harness::state.playback_closes;
}
}  // namespace boompi::playback

namespace boompi::voice_net {
bool open(const config::VoiceClientConfig&) {
  ++harness::state.link_opens;
  return harness::state.link_open_ok;
}
bool online() {
  return harness::state.connected;
}
bool uploading() {
  return harness::state.input_open;
}
bool poll(LinkEvent* event) {
  auto& q = harness::state.inbound;
  if (q.empty()) {
    return false;
  }
  *event = std::move(q.front());
  q.pop_front();
  if (event->kind == LinkEventKind::Online) {
    harness::state.connected = true;
  }
  if (event->kind == LinkEventKind::Offline) {
    harness::state.connected = false;
  }
  return true;
}
SendResult start(std::uint32_t gen, bool supersede) {
  harness::state.input_open = true;
  harness::state.input_frames = 0;
  return harness::Send({"start", gen, 0, supersede});
}
SendResult send(std::uint32_t gen, const std::int16_t* pcm) {
  const auto result = harness::Send({"pcm", gen, pcm[0]});
  if (result == SendResult::Ok) {
    ++harness::state.input_frames;
  }
  return result;
}
SendResult end(std::uint32_t gen) {
  harness::state.input_open = false;
  return harness::Send({"end", gen});
}
bool cancel(std::uint32_t gen, bool retract) {
  harness::state.input_open = false;
  harness::state.sent.push_back({"cancel", gen, 0, retract});
  if (!harness::state.stop_ok) {
    harness::state.connected = false;
  }
  return harness::state.stop_ok;
}
void close() noexcept {
  ++harness::state.link_closes;
}
bool save_wifi(const std::string& ssid, const std::string& password) {
  harness::state.saved_ssid = ssid;
  harness::state.saved_password = password;
  return !ssid.empty() && !password.empty();
}
}  // namespace boompi::voice_net

namespace boompi::ui {
struct DeviceUi::Impl {};
DeviceUi::~DeviceUi() noexcept = default;
std::uint8_t DeviceUi::LoadVolume(std::uint8_t fallback) noexcept {
  return fallback;
}
bool DeviceUi::Open() {
  return harness::state.ui_open_ok;
}
void DeviceUi::Show(const UiView& view) noexcept {
  harness::state.views.push_back(view);
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
  impl_ = nullptr;
  ++harness::state.ui_closes;
}
}  // namespace boompi::ui

int main() {
  using namespace harness;
  int failures = 0, checks = 0;
  auto require = [&](bool pass, const char* label) {
    ++checks;
    if (!pass) {
      ++failures;
      std::cerr << "FAIL: " << label << '\n';
    }
  };
  for (unsigned scenario = 0; scenario < 5; ++scenario) {
    state = {};
    state.audio_open_ok = scenario != 0;
    state.playback_open_ok = scenario != 1;
    state.capture_start_ok = scenario != 2;
    state.link_open_ok = scenario != 3;
    state.ui_open_ok = scenario != 4;
    boompi::config::VoiceClientConfig config;
    require(App_Init(config, &Now) == (scenario == 4), "startup errors and headless operation");
    const std::string error = App_GetError();
    App_Close();
    require(App_GetError() == error, "close preserves error text");
    require(state.audio_closes && state.playback_closes && state.link_closes && state.ui_closes,
            "partial initialization is cleaned up");
  }

#ifndef _WIN32
  // CLI 检查只改变当前测试进程环境；不打开真实设备或保存真实凭据。
  const std::array<const char*, 4> keys{"BOOMPI_DEVICE_ID", "BOOMPI_SERVER_IP",
                                        "BOOMPI_SERVER_PORT", "BOOMPI_SERVER_SPKI_SHA256"};
  std::array<std::optional<std::string>, 4> original;
  for (std::size_t i = 0; i < keys.size(); ++i) {
    if (const char* value = std::getenv(keys[i])) {
      original[i] = value;
    }
    unsetenv(keys[i]);
  }
  auto command = [](std::vector<std::string> args, const std::string& input = "") {
    std::vector<char*> argv;
    for (auto& arg : args) {
      argv.push_back(arg.data());
    }
    argv.push_back(nullptr);
    std::istringstream supplied(input);
    std::ostringstream output;
    auto* previous_in = std::cin.rdbuf(supplied.rdbuf());
    auto* previous_out = std::cout.rdbuf(output.rdbuf());
    auto* previous_error = std::cerr.rdbuf(output.rdbuf());
    std::cin.clear();
    const int result = BoompiClientMain(static_cast<int>(args.size()), argv.data());
    std::cin.rdbuf(previous_in);
    std::cout.rdbuf(previous_out);
    std::cerr.rdbuf(previous_error);
    std::cin.clear();
    return result;
  };
  state = {};
  require(command({"client", "--unknown"}) == EXIT_FAILURE && state.audio_opens == 0,
          "invalid CLI command never opens devices");
  require(command({"client", "--check-config", "extra"}) == EXIT_FAILURE,
          "extra CLI arguments are rejected");
  require(
      command({"client", "--voice-loop", "extra"}) == EXIT_FAILURE && state.audio_opens == 0,
      "extra voice-loop arguments never start the client");
  require(command({"client", "--check-config"}) == EXIT_FAILURE && state.audio_opens == 0,
          "missing configuration fails before initialization");
  require(command({"client", "--save-wifi"}, "classroom\nexample-password\n") == EXIT_SUCCESS &&
              state.saved_ssid == "classroom" && state.saved_password == "example-password",
          "Wi-Fi command reads both fields from standard input");
  require(command({"client", "--save-wifi"}, "classroom\n") == EXIT_FAILURE,
          "Wi-Fi command rejects incomplete input");
  setenv(keys[0], "00112233-4455-4677-8899-aabbccddeeff", 1);
  require(command({"client", "--check-config"}) == EXIT_SUCCESS && state.audio_opens == 0,
          "configuration check never starts audio");
  state = {};
  state.audio_open_ok = false;
  require(command({"client"}) == EXIT_FAILURE && state.audio_closes > 0,
          "default run propagates initialization failure and cleans up");
  state = {};
  state.healthy = false;
  require(
      command({"client"}) == EXIT_FAILURE && state.audio_closes > 0 && state.link_closes > 0,
      "runtime audio failure exits the main loop and closes modules");
  state = {};
  state.throw_on_audio_open = true;
  require(command({"client"}) == EXIT_FAILURE && state.audio_closes > 0 &&
              std::string(App_GetError()) == "application initialization failed",
          "startup exception becomes an application error and is cleaned up");
  state = {};
  state.throw_on_audio_process = true;
  require(command({"client"}) == EXIT_FAILURE && state.audio_closes > 0 &&
              state.link_closes > 0 &&
              std::string(App_GetError()) == "application processing failed",
          "processing exception becomes an application error and is cleaned up");
  state = {};
  stop_requested = 1;
  require(command({"client", "--voice-loop"}) == EXIT_SUCCESS && state.audio_opens == 1 &&
              state.audio_closes > 0 && state.link_closes > 0 && state.next == 0,
          "normal CLI run follows initialize run close");
  stop_requested = 0;
  for (std::size_t i = 0; i < keys.size(); ++i) {
    if (original[i]) {
      setenv(keys[i], original[i]->c_str(), 1);
    } else {
      unsetenv(keys[i]);
    }
  }
#endif

  auto steps = Question();
  require(Run(steps) && state.sent.size() == 43 && state.sent[0].kind == "start" &&
              state.sent[1].kind == "pcm" && state.sent[1].sample == 11 &&
              state.sent[2].kind == "pcm" && state.sent[2].sample == 12 &&
              state.sent[42].kind == "end",
          "real speech drives START PCM PCM END without duplicate or missing frame");
  steps.push_back(N(Reply()));
  steps.push_back(N(Net(LinkEventKind::Done, 1)));
  steps.push_back(Delay(20));
  require(Run(steps) && state.finished == std::vector<std::uint32_t>{1} && state.resets == 1 &&
              state.views.back().state == DeviceUiState::Speaking,
          "DONE closes playback input but cannot open follow-up before drain");
  steps.push_back(Drained(1));
  require(
      Run(steps) && state.resets == 2 && state.views.back().state == DeviceUiState::Listening,
      "matching physical drain opens follow-up once");

  steps = Question();
  steps.push_back(N(Reply()));
  auto text = Net(LinkEventKind::Text, 1);
  text.text = "tail text";
  steps.push_back(N(text));
  steps.push_back(Delay(20));
  require(Run(steps) && state.finished.empty() && state.resets == 1 &&
              std::string(state.views.back().text.data()) == "tail text",
          "short audio does not finish input or drop late text before DONE");

  steps = Question();
  steps.push_back(User(UiActionKind::Interrupt));
  steps.push_back(N(Reply()));
  steps.push_back(N(Net(LinkEventKind::Done, 1)));
  steps.push_back(Delay(20));
  require(
      Run(steps) && Count("cancel") == 1 && state.sent.back().retract && state.played.empty(),
      "input END leaves waiting answer cancellable and old audio isolated");

  steps = Question();
  steps.push_back(N(Reply()));
  steps.push_back(Delay(20));
  for (int i = 0; i < 30; ++i) {
    auto frame = Sound(90);
    frame.reference_active = true;
    steps.push_back(A(frame));
  }
  for (int id = 100; id < 115; ++id) {
    auto frame = Sound(id);
    frame.reference_active = id < 106;
    steps.push_back(A(frame));
  }
  for (int i = 0; i < 35; ++i) {
    steps.push_back(A(Sound(0, false, true)));
  }
  steps.push_back(N(Net(LinkEventKind::Done, 1)));
  steps.push_back(N(Reply(2)));
  steps.push_back(Delay(20));
  steps.push_back(Drained(1));
  require(Run(steps) && Count("start") == 2 && Count("end") == 2 && Count("pcm") == 91 &&
              state.sent[43].kind == "start" && state.sent[43].generation == 2 &&
              state.sent[43].retract && state.sent[44].sample == 100 &&
              state.sent[58].sample == 114,
          "confirmed barge stops old playback and submits all retained near speech");
  require(state.views.back().state == DeviceUiState::Speaking && state.finished.empty(),
          "old DONE and old physical completion cannot finish new answer");
  require(std::find(state.scales.begin(), state.scales.end(), 0.0F) != state.scales.end(),
          "barge keeps mute-and-confirm probe rather than cancel on first VAD");

  steps = Question();
  steps.push_back(N(Net(LinkEventKind::Done, 1)));
  steps.push_back(Delay(20));
  steps.push_back(Delay(3001));
  require(Run(steps) && state.views.back().state == DeviceUiState::Idle,
          "text-only answer and follow-up timeout");
  auto wake = Sound(0);
  wake.wake = true;
  steps = {A(wake), User(UiActionKind::Wake)};
  for (unsigned i = 0; i < 6; ++i) {
    steps.push_back(A(Sound(77)));
  }
  require(
      Run(steps) && state.sent.empty() && state.views.back().state == DeviceUiState::Offline,
      "offline wake and touch cannot start an upload");
  require(Run({N(Net(LinkEventKind::Online)), A(wake), Delay(6001)}) && state.sent.empty() &&
              state.views.back().state == DeviceUiState::Idle,
          "wake timeout never creates empty START");

  steps = Question();
  steps.push_back(Delay(30001));
  require(Run(steps) && Count("cancel") == 1, "response timeout cancels turn");
  require(
      Run(Question(), {SendResult::Backpressure}) && Count("pcm") == 0 && Count("cancel") == 1,
      "START backpressure sends no PCM");
  require(Run(Question(), {SendResult::Ok, SendResult::Ok, SendResult::Backpressure}) &&
              Count("end") == 0 && Count("cancel") == 1,
          "PCM backpressure cancels instead of committing truncated input");
  require(Run(Question(), EndBackpressure()) && Count("cancel") == 1,
          "END backpressure remains a failure");
  require(Run(Question(), {SendResult::Disconnected}) &&
              state.views.back().state == DeviceUiState::Offline,
          "disconnected START goes offline");
  require(Run(Question(), {SendResult::Backpressure}, true, false) &&
              state.views.back().state == DeviceUiState::Offline,
          "failed CANCEL goes offline");

  steps = Question();
  steps.push_back(N(Reply()));
  steps.push_back(Delay(20));
  require(Run(steps, {}, false) && Count("cancel") == 1 && state.sent.back().retract,
          "full playback queue retires unheard answer");
  auto device_failure = steps;
  Step failed;
  failed.output = boompi::playback::Status{1, boompi::playback::State::Failed};
  device_failure.push_back(failed);
  device_failure.push_back(N(Net(LinkEventKind::Offline)));
  require(!Run(device_failure) && std::string(App_GetError()) == "scripted playback error",
          "hardware failure is not hidden by cancellation or offline transition");
  steps.push_back(N(Net(LinkEventKind::Done, 1)));
  steps.push_back(Delay(20));
  steps.push_back(Delay(3001));
  require(Run(steps) && Count("cancel") == 1, "physical drain timeout stays bounded");

  steps = Question();
  steps.pop_back();
  auto gap = Sound(99);
  gap.discontinuity = true;
  steps.push_back(A(gap));
  require(Run(steps) && Count("end") == 0 && Count("cancel") == 1,
          "capture discontinuity never commits damaged input");
  steps = Question();
  steps.pop_back();
  steps.push_back(N(Net(LinkEventKind::Error, 1)));
  steps.push_back(A(Sound(99, false, true)));
  require(Run(steps) && Count("pcm") == 40 && Count("end") == 0 && Count("cancel") == 1,
          "provider error during upload stops immediately");

  steps = Question();
  steps.push_back(N(Net(LinkEventKind::Offline)));
  steps.push_back(Delay(20));
  steps.push_back(N(Net(LinkEventKind::Online)));
  steps.push_back(A(wake));
  for (int i = 0; i < 6; ++i) {
    steps.push_back(A(Sound(55)));
  }
  for (int i = 0; i < 35; ++i) {
    steps.push_back(A(Sound(0, false, true)));
  }
  steps.push_back(N(Reply(1)));
  steps.push_back(Delay(20));
  require(Run(steps) && Count("pcm") == 82 && state.sent.back().generation == 2 &&
              state.played.empty(),
          "reconnect uses a new generation without retransmitting or accepting old audio");
  steps = Question();
  steps.push_back(User(UiActionKind::Volume, 0));
  require(Run(steps) && state.views.back().volume == 0,
          "volume remains an independent user preference");

  steps = {N(Net(LinkEventKind::Online)), A(wake), A(Sound(1, true))};
  for (int i = 0; i < 3001; ++i) {
    auto step = A(Sound(2));
    step.elapsed_ms = 0;
    steps.push_back(step);
  }
  require(Run(steps) && Count("pcm") == 3000 && Count("end") == 1,
          "60-second media bound sends exactly one END after the last allowed PCM");

  std::cout << "voice flow: " << checks << " behavioral checks, " << failures << " failures\n";
  return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
