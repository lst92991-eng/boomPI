// 直接执行生产 VoiceApp；假模块仅记录 I/O，时钟可控，不复制业务状态机。
#include "../src/application/voice_client.cpp"
#include <deque>
#include <iostream>
#include <optional>
#include <vector>

namespace harness {
using boompi::audio::AudioEvent;
using boompi::audio::AudioEventKind;
using boompi::network::LinkEvent;
using boompi::network::LinkEventKind;
using boompi::network::SendResult;
using boompi::ui::UiAction;
struct Step {
  std::optional<AudioEvent> audio;
  std::vector<LinkEvent> network;
  std::optional<UiAction> user;
  int elapsed_ms{20};
};
struct Packet { std::uint32_t generation; bool start, end, supersede; std::int16_t sample; };
struct State {
  std::vector<Step> steps;
  std::size_t next{0};
  std::deque<LinkEvent> inbound;
  std::optional<UiAction> action;
  std::vector<Packet> packets;
  std::vector<std::pair<std::uint32_t, bool>> stops;
  std::vector<boompi::ui::UiView> views;
  std::vector<std::uint32_t> played;
  std::vector<bool> listen;
  std::vector<SendResult> results;
  std::uint64_t time_ms{0};
  volatile std::sig_atomic_t stop{0};
  bool healthy{true}, play_ok{true}, stop_ok{true};
} state;
auto Now() { return std::chrono::steady_clock::time_point(std::chrono::milliseconds(state.time_ms)); }
AudioEvent Sound(AudioEventKind kind, int sample = 0, bool end = false, unsigned gen = 0) {
  AudioEvent e; e.kind = kind; e.pcm.fill(static_cast<std::int16_t>(sample)); e.end = end; e.generation = gen;
  return e;
}
LinkEvent Net(LinkEventKind kind, unsigned gen = 0) {
  LinkEvent e; e.kind = kind; e.generation = gen; return e;
}
Step A(AudioEvent event) { Step s; s.audio = event; return s; }
Step N(LinkEvent event) { Step s; s.network.push_back(event); return s; }
Step Delay(int ms) { Step s; s.elapsed_ms = ms; return s; }
Step User(boompi::ui::UiActionKind kind, std::uint8_t volume = 60) {
  Step s; s.user = UiAction{kind, volume}; return s;
}
std::vector<Step> Question() {
  return {N(Net(LinkEventKind::Online)), A(Sound(AudioEventKind::Wake)),
          A(Sound(AudioEventKind::SpeechStart)), A(Sound(AudioEventKind::Pcm, 11)),
          A(Sound(AudioEventKind::Pcm, 22, true))};
}
LinkEvent Reply(unsigned gen = 1) {
  auto e = Net(LinkEventKind::Audio, gen);
  e.audio_size = 960; e.start = true; e.end = true; return e;
}
bool Run(std::vector<Step> steps, std::vector<SendResult> results = {}, bool play_ok = true, bool stop_ok = true) {
  state = {};
  state.steps = std::move(steps); state.results = std::move(results);
  state.play_ok = play_ok; state.stop_ok = stop_ok;
  boompi::config::VoiceClientConfig config; config.device_id = "00112233-4455-4677-8899-aabbccddeeff";
  std::string error;
  return boompi::application::VoiceApp(config, &error, &Now).Run(&state.stop);
}
}  // namespace harness

namespace boompi::audio {
struct VoiceAudio::Impl {};
VoiceAudio::~VoiceAudio() noexcept { Close(); }
bool VoiceAudio::Open(std::uint8_t) { return true; }
bool VoiceAudio::Poll(AudioEvent* event, std::chrono::milliseconds) {
  auto& s = harness::state;
  if (s.next == s.steps.size()) { s.stop = 1; return false; }
  auto step = s.steps[s.next++];
  s.time_ms += static_cast<unsigned>(step.elapsed_ms);
  for (auto& incoming : step.network) s.inbound.push_back(incoming);
  s.action = step.user;
  if (!step.audio) return false;
  *event = *step.audio;
  return true;
}
bool VoiceAudio::Listen(bool follow_up) { harness::state.listen.push_back(follow_up); return true; }
bool VoiceAudio::Play(std::uint32_t gen, const std::uint8_t*, std::size_t,
                       std::uint32_t, bool, bool) {
  harness::state.played.push_back(gen); return harness::state.play_ok;
}
void VoiceAudio::StopPlayback() {}
void VoiceAudio::CancelInput() {}
void VoiceAudio::SetVolume(std::uint8_t) {}
bool VoiceAudio::healthy() const { return harness::state.healthy; }
std::string VoiceAudio::last_error() const { return "scripted audio error"; }
void VoiceAudio::Close() noexcept {}
}
namespace boompi::network {
class VoiceLink::Impl {};
VoiceLink::VoiceLink() : impl_(std::make_unique<Impl>()) {}
VoiceLink::~VoiceLink() = default;
bool VoiceLink::Open(const LinkConfig&) { return true; }
bool VoiceLink::Poll(LinkEvent* event) {
  auto& q = harness::state.inbound;
  if (q.empty()) return false;
  *event = std::move(q.front()); q.pop_front(); return true;
}
SendResult VoiceLink::SendAudio(std::uint32_t gen, const std::int16_t* pcm,
                                bool start, bool end, bool supersede) {
  auto& s = harness::state;
  const auto index = s.packets.size();
  s.packets.push_back({gen,start,end,supersede,pcm[0]});
  return index < s.results.size() ? s.results[index] : SendResult::Ok;
}
bool VoiceLink::Stop(std::uint32_t gen, bool retract) {
  harness::state.stops.emplace_back(gen,retract); return harness::state.stop_ok;
}
void VoiceLink::Close() noexcept {}
}
namespace boompi::ui {
struct DeviceUi::Impl {};
DeviceUi::~DeviceUi() noexcept = default;
std::uint8_t DeviceUi::LoadVolume(std::uint8_t fallback) noexcept { return fallback; }
bool DeviceUi::Open() { return true; }
void DeviceUi::Show(const UiView& view) noexcept { harness::state.views.push_back(view); }
bool DeviceUi::Poll(UiAction* action) noexcept {
  if (!harness::state.action) return false;
  *action = *harness::state.action; harness::state.action.reset(); return true;
}
void DeviceUi::Close() noexcept {}
}

int main() {
  using namespace harness;
  using boompi::ui::DeviceUiState;
  using boompi::ui::UiActionKind;
  int failures = 0, checks = 0;
  auto require = [&](bool pass, const char* label) {
    ++checks;
    if (!pass) { ++failures; std::cerr << "FAIL: " << label << '\n'; }
  };
  auto steps = Question();
  steps.push_back(N(Reply()));
  steps.push_back(N(Net(LinkEventKind::Done,1)));
  steps.push_back(A(Sound(AudioEventKind::PlaybackDone,0,false,1)));
  require(Run(steps), "normal execution");
  require(state.packets.size()==2 && state.packets[0].start && !state.packets[0].end &&
          state.packets[1].end && state.packets[0].sample==11, "START/PCM/END preserve audio");
  require(state.listen.size()==2 && state.listen.back(), "physical end enters follow-up");

  steps = Question(); steps.push_back(N(Reply()));
  steps.push_back(Delay(20)); steps.push_back(A(Sound(AudioEventKind::Barge)));
  steps.push_back(A(Sound(AudioEventKind::Pcm,33,true)));
  steps.push_back(N(Net(LinkEventKind::Text,1)));
  steps.push_back(A(Sound(AudioEventKind::PlaybackDone,0,false,1)));
  steps.push_back(N(Reply(2))); steps.push_back(Delay(20));
  require(Run(steps), "barge execution");
  require(state.packets.size()==3 && state.packets.back().generation==2 &&
          state.packets.back().supersede && state.packets.back().start &&
          state.packets.back().end && state.stops.empty(), "short barge starts immediately without ACK");
  require(state.played.size()==2 && state.views.back().state==DeviceUiState::kSpeaking,
          "old completion cannot terminate new reply");

  steps = Question(); steps.push_back(N(Reply())); steps.push_back(Delay(20));
  steps.push_back(User(UiActionKind::Interrupt)); steps.push_back(A(Sound(AudioEventKind::SpeechStart)));
  steps.push_back(A(Sound(AudioEventKind::Pcm,44,true)));
  require(Run(steps), "touch stop then question");
  require(state.stops.size()==1 && state.stops[0].second &&
          state.packets.back().generation==3 && !state.packets.back().supersede,
          "STOP uses new fence; next ordinary question remains normal");

  steps = Question(); steps.push_back(N(Net(LinkEventKind::Done,1)));
  steps.push_back(Delay(20)); steps.push_back(Delay(3001));
  require(Run(steps) && state.views.back().state==DeviceUiState::kIdle, "text-only done and follow-up timeout");
  steps = {N(Net(LinkEventKind::Online)), A(Sound(AudioEventKind::Wake)), Delay(6001)};
  require(Run(steps) && state.packets.empty() && state.views.back().state==DeviceUiState::kIdle,
          "wake timeout produces no empty turn");

  steps = Question(); steps.push_back(Delay(30001));
  require(Run(steps) && state.stops.size()==1 && !state.stops[0].second, "response timeout stops generation");
  steps = Question();
  require(Run(steps,{SendResult::Backpressure}) && state.stops.size()==1,
          "uplink overflow stops without fabricated END");
  require(Run(Question(),{SendResult::Disconnected}) && state.stops.empty() &&
          state.views.back().state==DeviceUiState::kOffline, "disconnect cancels upload");
  require(Run(Question(),{SendResult::Backpressure},true,false) &&
          state.views.back().state==DeviceUiState::kOffline, "failed STOP becomes offline");

  steps = Question(); steps.push_back(N(Reply())); steps.push_back(Delay(20));
  require(Run(steps,{},false) && state.stops.size()==1 && state.stops[0].second,
          "TTS rejected retires unheard answer");

  steps = Question(); steps.push_back(N(Net(LinkEventKind::Offline))); steps.push_back(Delay(20));
  steps.push_back(N(Net(LinkEventKind::Online))); steps.push_back(A(Sound(AudioEventKind::Wake)));
  steps.push_back(A(Sound(AudioEventKind::SpeechStart))); steps.push_back(A(Sound(AudioEventKind::Pcm,55,true)));
  require(Run(steps) && state.packets.size()==3 && state.packets.back().generation==2,
          "reconnect never replays old speech");

  steps = Question(); steps.pop_back(); steps.push_back(A(Sound(AudioEventKind::Fault)));
  require(Run(steps) && state.stops.size()==1 && !state.packets.back().end,
          "capture gap never commits damaged input");
  steps = Question(); steps.pop_back();
  steps.push_back(N(Net(LinkEventKind::Error,1)));
  steps.push_back(A(Sound(AudioEventKind::Pcm,99,true)));
  require(Run(steps) && state.stops.size()==1 && state.packets.size()==1 &&
          !state.packets.back().end, "provider failure during upload cancels immediately");
  steps = Question(); steps.push_back(User(UiActionKind::Volume,0));
  require(Run(steps) && state.views.back().volume==0, "volume remains UI preference");

  steps = Question(); steps.push_back(N(Reply())); steps.push_back(Delay(20));
  steps.push_back(Delay(3001));
  require(Run(steps) && state.stops.size()==1 && state.stops[0].second, "stalled physical drain bounded");

  steps = {N(Net(LinkEventKind::Online)), A(Sound(AudioEventKind::Wake)),
           A(Sound(AudioEventKind::SpeechStart))};
  for (int i=0;i<3001;++i) { auto step=A(Sound(AudioEventKind::Pcm)); step.elapsed_ms=0; steps.push_back(step); }
  require(Run(steps) && state.packets.size()==3000 && state.packets.back().end,
          "60-second media limit sends exactly one END");
  std::cout << "voice app: " << checks << " behavioral checks, " << failures << " failures\n";
  return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
