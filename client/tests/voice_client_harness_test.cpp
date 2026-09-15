/**
 * @file voice_client_harness_test.cpp
 * @brief 直接执行生产应用模块，用脚本事件和可控时钟验证完整问答/插话状态转移。
 *
 * Question 建立正常上传路径，场景追加网络/音频/UI 事件；Run 调用真实主循环，
 * 假模块记录发包、STOP、播放与显示结果。网络收发、声学检测和硬件不在本测试执行。
 */
#include <csignal>
#include <cstdlib>
#include <deque>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <vector>

#include "boompi/application/voice_client.h"
#include "boompi/audio/voice_audio.h"
#include "boompi/network/voice_link.h"
#include "boompi/ui/device_ui.h"

// 复用本机既有测试入口，Linux 下直接执行实际 CLI，不建立第二套入口实现。
#ifndef _WIN32
#define main BoompiClientMain
#include "../apps/boompi_client/main.cpp"
#undef main
#endif

namespace harness {
using boompi::audio::AudioEvent;
using boompi::audio::AudioEventKind;
using boompi::audio::ListenMode;
using boompi::network::LinkEvent;
using boompi::network::LinkEventKind;
using boompi::network::SendResult;
using boompi::ui::UiAction;
/**
 * @brief 一次假 VoiceAudio::ProcessEvents 调用推进的事件和时间。
 * 网络事件在本步压入队列，下一轮 App_Process 先消费网络；UI 动作在本轮音频处理后读取。
 */
struct Step {
  std::vector<AudioEvent> audio;
  std::vector<LinkEvent> network;
  std::optional<UiAction> user;
  int elapsed_ms{20};
};
/// 记录上传首个样本和协议标志，用于检查 PCM 顺序及 START/END/SUPERSEDE 组合。
struct Packet {
  std::uint32_t generation;
  bool start, end, supersede;
  std::int16_t sample;
};
/// 场景脚本、I/O 记录和假时钟；整个状态机测试由单线程推进，无真实 worker。
struct State {
  std::vector<Step> steps;
  std::size_t next{0};
  std::deque<LinkEvent> inbound;
  std::optional<UiAction> action;
  std::vector<Packet> packets;
  std::vector<std::pair<std::uint32_t, bool>> stops;
  std::vector<boompi::ui::UiView> views;
  std::vector<std::uint32_t> played;
  std::vector<ListenMode> listen;
  std::vector<SendResult> results;
  std::uint64_t time_ms{0};
  volatile std::sig_atomic_t stop{0};
  bool healthy{true}, play_ok{true}, stop_ok{true};
  bool audio_open_ok{true}, link_open_ok{true}, ui_open_ok{true};
  bool throw_on_audio_open{false}, throw_on_audio_process{false};
  unsigned audio_opens{0}, link_opens{0}, audio_closes{0}, link_closes{0}, ui_closes{0};
  std::string saved_ssid, saved_password;
} state;
/** @brief 将脚本累计毫秒转换为单调时刻，令超时边界无需实际等待数十秒。 */
auto Now() {
  return std::chrono::steady_clock::time_point(std::chrono::milliseconds(state.time_ms));
}
/// 创建音频事件；常量样本区分不同输入帧，gen 用于模拟旧播放完成通知。
AudioEvent Sound(AudioEventKind kind, int sample = 0, bool end = false, unsigned gen = 0) {
  AudioEvent e;
  e.kind = kind;
  e.pcm.fill(static_cast<std::int16_t>(sample));
  e.end = end;
  e.generation = gen;
  return e;
}
/// 创建属于指定 generation 的网络事件，连接类事件使用默认 0。
LinkEvent Net(LinkEventKind kind, unsigned gen = 0) {
  LinkEvent e;
  e.kind = kind;
  e.generation = gen;
  return e;
}
// A/N/Delay/User 是场景构造辅助，只填写 Step，不提前执行产品行为。
Step A(AudioEvent event) {
  Step s;
  s.audio.push_back(event);
  return s;
}
Step N(LinkEvent event) {
  Step s;
  s.network.push_back(event);
  return s;
}
Step Delay(int ms) {
  Step s;
  s.elapsed_ms = ms;
  return s;
}
Step User(boompi::ui::UiActionKind kind, std::uint8_t volume = 60) {
  Step s;
  s.user = UiAction{kind, volume};
  return s;
}
/** @brief 标准问题脚本：上线→唤醒→开口→两帧 PCM，第二帧结束输入。 */
std::vector<Step> Question() {
  Step admitted = A(Sound(AudioEventKind::SpeechStart));
  admitted.audio.push_back(Sound(AudioEventKind::Pcm, 11));
  return {N(Net(LinkEventKind::Online)), A(Sound(AudioEventKind::Wake)), admitted,
          A(Sound(AudioEventKind::Pcm, 22, true))};
}
/** @brief 一包带 START|END 的下行音频；物理播放完成需要场景另外发送 PlaybackDone。 */
LinkEvent Reply(unsigned gen = 1) {
  auto e = Net(LinkEventKind::Audio, gen);
  e.audio_size = boompi::audio::kTtsFrameSamples * sizeof(std::int16_t);
  e.start = true;
  e.end = true;
  return e;
}
/**
 * @brief 每次清空记录并执行真实应用模块，results 可按发包次序注入背压或断线。
 * 通过脚本耗尽设置 stop 结束；play_ok/stop_ok 只控制假 I/O 结果，不更改业务状态。
 */
bool Run(std::vector<Step> steps, std::vector<SendResult> results = {}, bool play_ok = true,
         bool stop_ok = true) {
  state = {};
  state.steps = std::move(steps);
  state.results = std::move(results);
  state.play_ok = play_ok;
  state.stop_ok = stop_ok;
  boompi::config::VoiceClientConfig config;
  config.device_id = "00112233-4455-4677-8899-aabbccddeeff";
  if (!App_Init(config, &Now)) {
    return false;
  }
  bool succeeded = true;
  while (state.stop == 0 && succeeded) {
    succeeded = App_Process();
  }
  App_Close();
  return succeeded;
}
}  // namespace harness

// 以下同名实现由本测试单独链接，提供真实应用模块 所需的 I/O 边界，不编译生产模块。
namespace boompi::audio {
struct VoiceAudio::Impl {};
VoiceAudio::~VoiceAudio() noexcept = default;
bool VoiceAudio::Open(std::uint8_t) {
  ++harness::state.audio_opens;
  if (harness::state.throw_on_audio_open) {
    throw std::runtime_error("scripted startup exception");
  }
  return harness::state.audio_open_ok;
}
/** @brief 每次消费一步脚本并推进假时钟；无事件时返回空批次。 */
void VoiceAudio::ProcessEvents(std::vector<AudioEvent>& events, std::chrono::milliseconds) {
  events.clear();
  auto& s = harness::state;
  if (s.throw_on_audio_process) {
    throw std::runtime_error("scripted processing exception");
  }
  if (s.next == s.steps.size()) {
    s.stop = 1;
    return;
  }
  auto step = s.steps[s.next++];
  s.time_ms += static_cast<unsigned>(step.elapsed_ms);
  for (auto& incoming : step.network) {
    s.inbound.push_back(incoming);
  }
  s.action = step.user;
  events = std::move(step.audio);
}

bool VoiceAudio::Listen(ListenMode mode) {
  harness::state.listen.push_back(mode);
  return true;
}
bool VoiceAudio::Play(std::uint32_t gen, const std::uint8_t*, std::size_t, std::uint32_t, bool,
                      bool) {
  harness::state.played.push_back(gen);
  return harness::state.play_ok;
}
void VoiceAudio::StopPlayback() {}
void VoiceAudio::CancelInput() {}
void VoiceAudio::SetVolume(std::uint8_t) {}
bool VoiceAudio::IsHealthy() const {
  return harness::state.healthy;
}
std::string VoiceAudio::LastError() const {
  return "scripted audio error";
}
void VoiceAudio::Close() noexcept {
  ++harness::state.audio_closes;
}
}  // namespace boompi::audio
namespace boompi::network {
class VoiceLink::Impl {};
VoiceLink::VoiceLink() : impl_(std::make_unique<Impl>()) {}
VoiceLink::~VoiceLink() = default;
bool VoiceLink::Open(const config::VoiceClientConfig&) {
  ++harness::state.link_opens;
  return harness::state.link_open_ok;
}
bool VoiceLink::PollEvent(LinkEvent* event) {
  auto& q = harness::state.inbound;
  if (q.empty()) {
    return false;
  }
  *event = std::move(q.front());
  q.pop_front();
  return true;
}
SendResult VoiceLink::SendAudio(std::uint32_t gen, const std::int16_t* pcm, bool start,
                                bool end, bool supersede) {
  // 记录的是发送尝试，包含被注入错误拒绝的那次调用，断言需同时检查返回结果的影响。
  auto& s = harness::state;
  const auto index = s.packets.size();
  s.packets.push_back({gen, start, end, supersede, pcm[0]});
  return index < s.results.size() ? s.results[index] : SendResult::Ok;
}
bool VoiceLink::Stop(std::uint32_t gen, bool retract) {
  harness::state.stops.emplace_back(gen, retract);
  return harness::state.stop_ok;
}
void VoiceLink::Close() noexcept {
  ++harness::state.link_closes;
}
bool SaveWifi(const std::string& ssid, const std::string& password) {
  harness::state.saved_ssid = ssid;
  harness::state.saved_password = password;
  return !ssid.empty() && !password.empty();
}
}  // namespace boompi::network
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
  ++harness::state.ui_closes;
}
}  // namespace boompi::ui

/** @brief 顺序构造正常问答与异常场景，核对发包标志、轮次隔离、状态显示和截止时间。 */
int main() {
  using namespace harness;
  using boompi::ui::DeviceUiState;
  using boompi::ui::UiActionKind;
  int failures = 0, checks = 0;
  auto require = [&](bool pass, const char* label) {
    ++checks;
    if (!pass) {
      ++failures;
      std::cerr << "FAIL: " << label << '\n';
    }
  };
  // 显式生命周期：启动失败也回收已打开模块；无屏仍可运行语音。
  for (unsigned scenario = 0; scenario < 3; ++scenario) {
    state = {};
    state.stop = 1;
    state.audio_open_ok = scenario != 0;
    state.link_open_ok = scenario != 1;
    state.ui_open_ok = scenario != 2;
    boompi::config::VoiceClientConfig config;
    {
      const bool opened = App_Init(config, &Now);
      require(opened == (scenario == 2), "module startup failure is propagated");
      if (opened) {
        require(App_GetError()[0] == '\0', "headless startup has no fatal error");
      } else {
        require(App_GetError()[0] != '\0', "startup failure has an owned error message");
      }
      const std::string error = App_GetError();
      App_Close();
      require(App_GetError() == error, "closing preserves the application error");
    }
    require(state.audio_closes > 0 && state.link_closes > 0 && state.ui_closes > 0,
            "all lifecycle paths close modules");
    if (scenario == 0) {
      require(state.link_opens == 0, "audio failure does not start network");
    }
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
  // 正常链路：完成输入与回复后，只有匹配轮次的物理播放完成才能切入追问。
  auto steps = Question();
  steps.push_back(N(Reply()));
  steps.push_back(N(Net(LinkEventKind::Done, 1)));
  steps.push_back(A(Sound(AudioEventKind::PlaybackDone, 0, false, 1)));
  require(Run(steps), "normal execution");
  require(state.packets.size() == 2 && state.packets[0].start && !state.packets[0].end &&
              state.packets[1].end && state.packets[0].sample == 11,
          "START/PCM/END preserve audio");
  require(state.listen == std::vector<ListenMode>{ListenMode::Wake, ListenMode::FollowUp},
          "wake and physical end select the matching listen mode");

  steps = Question();
  steps.push_back(N(Reply()));
  steps.push_back(N(Net(LinkEventKind::Done, 1)));
  steps.push_back(Delay(20));
  // 单独收到网络 Done 不能冒充 PlaybackDone，否则扬声器仍有尾音时就会开始收追问。
  require(Run(steps) && state.listen == std::vector<ListenMode>{ListenMode::Wake} &&
              state.views.back().state == DeviceUiState::Speaking,
          "network END and done do not start follow-up before physical playback ends");

  steps = Question();
  steps.push_back(N(Reply()));
  steps.push_back(Delay(20));
  steps.push_back(A(Sound(AudioEventKind::PlaybackDone, 0, false, 1)));
  auto late_text = Net(LinkEventKind::Text, 1);
  late_text.text = "tail text";
  steps.push_back(N(late_text));
  steps.push_back(Delay(20));
  require(Run(steps) && state.listen == std::vector<ListenMode>{ListenMode::Wake} &&
              std::string(state.views.back().text.data()) == "tail text",
          "physical end before DONE still accepts text and waits for network completion");
  steps.push_back(N(Net(LinkEventKind::Done, 1)));
  steps.push_back(Delay(20));
  require(Run(steps) &&
              state.listen == std::vector<ListenMode>{ListenMode::Wake, ListenMode::FollowUp},
          "late DONE completes an already drained answer exactly once");

  steps = Question();
  steps.push_back(N(Reply()));
  steps.push_back(Delay(20));
  steps.push_back(A(Sound(AudioEventKind::PlaybackDone, 0, false, 1)));
  steps.push_back(N(Net(LinkEventKind::Error, 1)));
  steps.push_back(Delay(20));
  require(Run(steps) && state.stops.size() == 1 && state.stops[0].second,
          "provider failure after physical end is still handled before DONE");

  steps = Question();
  steps.push_back(N(Reply()));
  steps.push_back(Delay(20));
  steps.push_back(A(Sound(AudioEventKind::Barge, 0, false, 1)));
  // 新插话是一轮正常输入；旧文本/旧完成随后到达，也不能终止 generation 2 的回答。
  steps.back().audio.push_back(Sound(AudioEventKind::Pcm, 33, true));
  steps.push_back(N(Net(LinkEventKind::Text, 1)));
  steps.push_back(A(Sound(AudioEventKind::PlaybackDone, 0, false, 1)));
  steps.push_back(N(Reply(2)));
  steps.push_back(Delay(20));
  require(Run(steps), "barge execution");
  require(state.packets.size() == 3 && state.packets.back().generation == 2 &&
              state.packets.back().supersede && state.packets.back().start &&
              state.packets.back().end && state.stops.empty(),
          "short barge starts immediately without ACK");
  require(state.played.size() == 2 && state.views.back().state == DeviceUiState::Speaking,
          "old completion cannot terminate new reply");

  steps = Question();
  steps.push_back(N(Reply()));
  steps.push_back(Delay(20));
  steps.push_back(User(UiActionKind::Interrupt));
  // 触摸停止先消耗一个 STOP generation，接下来的普通追问再分配一个 generation。
  steps.push_back(A(Sound(AudioEventKind::SpeechStart)));
  steps.push_back(A(Sound(AudioEventKind::Pcm, 44, true)));
  require(Run(steps), "touch stop then question");
  require(state.stops.size() == 1 && state.stops[0].second &&
              state.packets.back().generation == 3 && !state.packets.back().supersede,
          "STOP uses new fence; next ordinary question remains normal");
  require(state.listen == std::vector<ListenMode>{ListenMode::Wake, ListenMode::FollowUp},
          "touch stop also opens follow-up listening");

  steps = Question();
  steps.push_back(User(UiActionKind::Interrupt));
  steps.push_back(N(Reply(1)));
  steps.push_back(N(Net(LinkEventKind::Done, 1)));
  steps.push_back(Delay(20));
  require(Run(steps) && state.stops.size() == 1 && state.stops[0].second &&
              state.played.empty() && state.views.back().state == DeviceUiState::Listening,
          "END leaves the waiting response cancellable and rejects late audio/done");

  steps = Question();
  steps.push_back(N(Reply()));
  steps.push_back(Delay(20));
  steps.push_back(User(UiActionKind::Interrupt));
  steps.push_back(A(Sound(AudioEventKind::SpeechStart)));
  steps.push_back(A(Sound(AudioEventKind::Pcm, 44, true)));
  steps.push_back(N(Reply(3)));
  steps.push_back(Delay(20));
  steps.push_back(A(Sound(AudioEventKind::Barge, 0, false, 1)));
  steps.back().audio.push_back(Sound(AudioEventKind::Pcm, 99, true));
  require(Run(steps) && state.packets.size() == 3 && state.packets.back().generation == 3 &&
              state.views.back().state == DeviceUiState::Speaking,
          "stale barge and its PCM cannot replace the current answer");

  steps = Question();
  steps.push_back(N(Net(LinkEventKind::Done, 1)));
  steps.push_back(Delay(20));
  steps.push_back(Delay(3001));
  require(Run(steps) && state.views.back().state == DeviceUiState::Idle,
          "text-only done and follow-up timeout");
  steps = {N(Net(LinkEventKind::Online)), A(Sound(AudioEventKind::Wake)), Delay(6001)};
  require(
      Run(steps) && state.packets.empty() && state.views.back().state == DeviceUiState::Idle,
      "wake timeout produces no empty turn");

  steps = Question();
  steps.push_back(Delay(30001));
  // 故障分支分别验证等待超时、入队背压、断线和 STOP 失败，不能统一伪装成输入 END。
  require(Run(steps) && state.stops.size() == 1 && !state.stops[0].second,
          "response timeout stops generation");
  steps = Question();
  require(Run(steps, {SendResult::Backpressure}) && state.stops.size() == 1,
          "uplink overflow stops without fabricated END");
  // 同一批中途背压，后续开始事件/PCM 也不能被当成新一轮输入。
  steps = Question();
  steps[2].audio.push_back(Sound(AudioEventKind::Pcm, 33));
  steps[2].audio.push_back(Sound(AudioEventKind::SpeechStart));
  steps[2].audio.push_back(Sound(AudioEventKind::Pcm, 44, true));
  require(Run(steps, {SendResult::Ok, SendResult::Backpressure}) && state.packets.size() == 2 &&
              state.packets.back().sample == 33 && !state.packets.back().end &&
              state.stops.size() == 1,
          "cancel discards the remaining event batch");
  require(Run(Question(), {SendResult::Disconnected}) && state.stops.empty() &&
              state.views.back().state == DeviceUiState::Offline,
          "disconnect cancels upload");
  require(Run(Question(), {SendResult::Backpressure}, true, false) &&
              state.views.back().state == DeviceUiState::Offline &&
              state.listen == std::vector<ListenMode>{ListenMode::Wake},
          "failed STOP becomes offline");

  steps = Question();
  steps.push_back(N(Reply()));
  steps.push_back(Delay(20));
  require(Run(steps, {}, false) && state.stops.size() == 1 && state.stops[0].second,
          "TTS rejected retires unheard answer");

  steps = Question();
  steps.push_back(N(Net(LinkEventKind::Offline)));
  // 重连从 Idle 重新开始，前一轮已发的语音只保留在测试记录中，不重新上传。
  steps.push_back(Delay(20));
  steps.push_back(N(Net(LinkEventKind::Online)));
  steps.push_back(A(Sound(AudioEventKind::Wake)));
  steps.push_back(A(Sound(AudioEventKind::SpeechStart)));
  steps.push_back(A(Sound(AudioEventKind::Pcm, 55, true)));
  require(Run(steps) && state.packets.size() == 3 && state.packets.back().generation == 2,
          "reconnect never replays old speech");

  steps = Question();
  steps.pop_back();
  steps.push_back(A(Sound(AudioEventKind::Fault)));
  // 在输入尚未结束时注入缺帧或服务端错误，剩余 PCM 不得补成一个成功提交的问题。
  require(Run(steps) && state.stops.size() == 1 && !state.packets.back().end,
          "capture gap never commits damaged input");
  steps = Question();
  steps.pop_back();
  steps.push_back(N(Net(LinkEventKind::Error, 1)));
  steps.push_back(A(Sound(AudioEventKind::Pcm, 99, true)));
  require(Run(steps) && state.stops.size() == 1 && state.packets.size() == 1 &&
              !state.packets.back().end,
          "provider failure during upload cancels immediately");
  steps = Question();
  steps.push_back(User(UiActionKind::Volume, 0));
  require(Run(steps) && state.views.back().volume == 0, "volume remains UI preference");

  steps = Question();
  steps.push_back(N(Reply()));
  steps.push_back(Delay(20));
  steps.push_back(Delay(3001));
  require(Run(steps) && state.stops.size() == 1 && state.stops[0].second,
          "stalled physical drain bounded");

  steps = {N(Net(LinkEventKind::Online)), A(Sound(AudioEventKind::Wake)),
           A(Sound(AudioEventKind::SpeechStart))};
  // 不推进时钟，只靠帧数触发 60 秒媒体上限，证明限制不依赖实际循环调度速度。
  for (int i = 0; i < 3001; ++i) {
    auto step = A(Sound(AudioEventKind::Pcm));
    step.elapsed_ms = 0;
    steps.push_back(step);
  }
  require(Run(steps) && state.packets.size() == 3000 && state.packets.back().end,
          "60-second media limit sends exactly one END");
  std::cout << "voice app: " << checks << " behavioral checks, " << failures << " failures\n";
  return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
