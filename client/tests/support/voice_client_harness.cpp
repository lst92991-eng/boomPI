#include "voice_client_harness.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <utility>

#include "boompi/network/network_bootstrap.h"

namespace boompi::tests {
namespace {

struct HarnessState final {
  std::mutex mutex;
  std::condition_variable changed;
  HarnessPlan plan;
  HarnessSnapshot snapshot;
  volatile std::sig_atomic_t* stop{nullptr};
  std::size_t capture_index{0U};
  std::size_t pcm_outcome_index{0U};
  std::uint64_t capture_sequence{0U};
  std::deque<ui::DeviceUiAction> actions;
  network::VoiceTransport* active_transport{nullptr};
  network::InboundHandler inbound;
  network::ErrorHandler transport_error;
  bool audio_open{false};
  bool audio_closed{false};
  bool playback_done{true};
  bool playback_failed{false};
  bool reconnect_pause_used{false};
};

HarnessState& State() {
  static HarnessState state;
  return state;
}

}  // namespace

void LoadHarness(HarnessPlan plan, volatile std::sig_atomic_t* const stop) {
  auto& state = State();
  std::lock_guard<std::mutex> lock(state.mutex);
  state.plan = std::move(plan);
  state.snapshot = {};
  state.stop = stop;
  state.capture_index = 0U;
  state.pcm_outcome_index = 0U;
  state.capture_sequence = 0U;
  state.actions.clear();
  state.active_transport = nullptr;
  state.inbound = {};
  state.transport_error = {};
  state.audio_open = false;
  state.audio_closed = false;
  state.playback_done = true;
  state.playback_failed = false;
  state.reconnect_pause_used = false;
  if (stop != nullptr) *stop = 0;
}

HarnessSnapshot ReadHarness() {
  auto& state = State();
  std::lock_guard<std::mutex> lock(state.mutex);
  return state.snapshot;
}

}  // namespace boompi::tests

namespace boompi::audio {

AudioEngine::~AudioEngine() noexcept { Close(); }

bool AudioEngine::Open(const AudioEngineConfig&) noexcept {
  auto& state = tests::State();
  std::lock_guard<std::mutex> lock(state.mutex);
  state.audio_open = true;
  state.audio_closed = false;
  state.playback_done = true;
  state.playback_failed = false;
  return true;
}

CaptureResult AudioEngine::Capture(
    CaptureFrame* const frame,
    const std::chrono::milliseconds timeout) noexcept {
  if (frame == nullptr) return CaptureResult::kFailed;
  auto& state = tests::State();
  tests::CaptureStep step;
  network::InboundHandler inbound;
  bool pause_for_reconnect = false;
  bool stop_after_timeout = false;
  bool scripted_timeout = false;
  unsigned reconnect_pause_ms = 0U;
  unsigned capture_delay_ms = 0U;
  {
    std::unique_lock<std::mutex> lock(state.mutex);
    if (!state.audio_open || state.audio_closed) return CaptureResult::kFailed;

    if (state.snapshot.network_bootstraps == 0U) {
      state.changed.wait_for(lock, std::chrono::milliseconds(100), [&state] {
        return state.snapshot.network_bootstraps != 0U;
      });
    }

    if (state.active_transport == nullptr) {
      if (state.snapshot.connections != 0U && !state.reconnect_pause_used) {
        state.reconnect_pause_used = true;
        pause_for_reconnect = true;
        reconnect_pause_ms = state.plan.reconnect_pause_ms;
      }
      frame->sequence = state.capture_sequence++;
    } else if (state.capture_index >= state.plan.capture.size()) {
      if (state.plan.stop_during_capture_timeout) {
        stop_after_timeout = true;
      } else {
        frame->sequence = state.capture_sequence++;
        if (state.stop != nullptr) *state.stop = 1;
      }
    } else {
      step = state.plan.capture[state.capture_index++];
      scripted_timeout = step.timeout;
      capture_delay_ms = step.delay_ms;
      state.playback_done = step.playback_done || state.playback_done;
      state.playback_failed = step.playback_failed;
      if (!scripted_timeout) {
        if (step.sequence_gap_before) ++state.capture_sequence;
        step.frame.sequence = state.capture_sequence++;
        *frame = step.frame;
        for (const auto action : step.actions) state.actions.push_back(action);
        inbound = state.inbound;
      }
    }
  }

  if (stop_after_timeout || scripted_timeout) {
    std::this_thread::sleep_for(timeout);
    std::lock_guard<std::mutex> lock(state.mutex);
    ++state.snapshot.capture_timeouts;
    if ((stop_after_timeout || step.stop_after) && state.stop != nullptr)
      *state.stop = 1;
    return CaptureResult::kTimeout;
  }

  if (capture_delay_ms != 0U)
    std::this_thread::sleep_for(std::chrono::milliseconds(capture_delay_ms));
  else if (pause_for_reconnect && reconnect_pause_ms != 0U)
    std::this_thread::sleep_for(std::chrono::milliseconds(reconnect_pause_ms));
  else if (!inbound)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));

  if (inbound) {
    for (auto& event : step.inbound) inbound(std::move(event));
    if (step.stop_after && state.stop != nullptr) *state.stop = 1;
  }
  return CaptureResult::kFrame;
}

bool AudioEngine::ResetListener() noexcept {
  auto& state = tests::State();
  std::lock_guard<std::mutex> lock(state.mutex);
  ++state.snapshot.listener_resets;
  return state.audio_open && !state.audio_closed;
}

bool AudioEngine::QueueTts24k(const std::uint8_t*, const std::size_t,
                              const std::uint64_t) noexcept {
  auto& state = tests::State();
  std::lock_guard<std::mutex> lock(state.mutex);
  return state.audio_open && !state.audio_closed;
}

bool AudioEngine::BeginPlayback() noexcept {
  auto& state = tests::State();
  std::lock_guard<std::mutex> lock(state.mutex);
  ++state.snapshot.playback_begins;
  state.playback_done = false;
  state.playback_failed = false;
  return true;
}

bool AudioEngine::EndPlayback() noexcept {
  auto& state = tests::State();
  std::lock_guard<std::mutex> lock(state.mutex);
  ++state.snapshot.playback_ends;
  return true;
}

void AudioEngine::SetPlaybackGain(float) noexcept {}

void AudioEngine::SetPlaybackScale(const float scale) noexcept {
  auto& state = tests::State();
  std::lock_guard<std::mutex> lock(state.mutex);
  state.snapshot.playback_scales.push_back(scale);
}

void AudioEngine::DropPlayback() noexcept {
  auto& state = tests::State();
  std::lock_guard<std::mutex> lock(state.mutex);
  ++state.snapshot.playback_drops;
  state.playback_done = true;
}

bool AudioEngine::playback_done() const noexcept {
  auto& state = tests::State();
  std::lock_guard<std::mutex> lock(state.mutex);
  return state.playback_done;
}

bool AudioEngine::playback_failed() const noexcept {
  auto& state = tests::State();
  std::lock_guard<std::mutex> lock(state.mutex);
  return state.playback_failed;
}

std::string AudioEngine::last_error() const { return "scripted audio failure"; }

void AudioEngine::Close() noexcept {
  auto& state = tests::State();
  std::lock_guard<std::mutex> lock(state.mutex);
  state.audio_closed = true;
  state.audio_open = false;
  state.playback_done = true;
}

}  // namespace boompi::audio

namespace boompi::network {

class VoiceTransport::Impl final {
 public:
  bool connected{false};
  bool closed{false};
};

VoiceTransport::VoiceTransport() : impl_(std::make_unique<Impl>()) {}
VoiceTransport::~VoiceTransport() { Close(); }

bool VoiceTransport::Connect(TransportConfig config, InboundHandler inbound,
                             ErrorHandler error) {
  auto& state = tests::State();
  std::lock_guard<std::mutex> lock(state.mutex);
  impl_->connected = true;
  impl_->closed = false;
  state.active_transport = this;
  state.inbound = std::move(inbound);
  state.transport_error = std::move(error);
  state.snapshot.connected_host = std::move(config.host);
  state.snapshot.connected_port = config.port;
  state.snapshot.connected_spki = std::move(config.spki_sha256_base64);
  ++state.snapshot.connections;
  state.changed.notify_all();
  return true;
}

bool VoiceTransport::SendControl(const Control& control) {
  auto& state = tests::State();
  InboundHandler inbound;
  Inbound hello;
  {
    std::lock_guard<std::mutex> lock(state.mutex);
    if (!impl_->connected || impl_->closed) return false;
    state.snapshot.controls.push_back(control);
    if (control.kind == ControlKind::kHello && state.plan.acknowledge_hello) {
      hello.kind = InboundKind::kHelloAck;
      hello.ids.session = static_cast<std::uint32_t>(100U + state.snapshot.connections);
      hello.ids.epoch = static_cast<std::uint32_t>(state.snapshot.connections);
      inbound = state.inbound;
    }
  }
  if (inbound) inbound(std::move(hello));
  return true;
}

SendOutcome VoiceTransport::SendPcm64(const Pcm64Header& header,
                                      const std::uint8_t* const payload,
                                      const std::size_t bytes) {
  auto& state = tests::State();
  std::lock_guard<std::mutex> lock(state.mutex);
  if (!impl_->connected || impl_->closed) return SendOutcome::kRejected;
  tests::PcmSend send{};
  send.header = header;
  if (payload != nullptr) send.payload.assign(payload, payload + bytes);
  state.snapshot.pcm.push_back(std::move(send));
  if (state.pcm_outcome_index < state.plan.pcm_outcomes.size())
    return state.plan.pcm_outcomes[state.pcm_outcome_index++];
  return SendOutcome::kOk;
}

void VoiceTransport::Close() noexcept {
  if (!impl_ || impl_->closed) return;
  auto& state = tests::State();
  std::lock_guard<std::mutex> lock(state.mutex);
  impl_->closed = true;
  impl_->connected = false;
  ++state.snapshot.transport_closes;
  if (state.active_transport == this) {
    state.active_transport = nullptr;
    state.inbound = {};
    state.transport_error = {};
  }
}

bool VoiceTransport::connected() const noexcept {
  auto& state = tests::State();
  std::lock_guard<std::mutex> lock(state.mutex);
  return impl_ && impl_->connected && !impl_->closed &&
         state.active_transport == this;
}

bool NetworkBootstrap::SaveWifi(const std::string&, const std::string&) {
  return true;
}

bool NetworkBootstrap::SaveServer(const NetworkBootstrapResult&) { return true; }

bool NetworkBootstrap::Start(const NetworkBootstrapResult* const explicit_server,
                             NetworkBootstrapResult* const output,
                             const std::atomic<bool>* const stop) {
  if (output == nullptr ||
      (stop != nullptr && stop->load(std::memory_order_acquire)))
    return false;
  auto& state = tests::State();
  {
    std::lock_guard<std::mutex> lock(state.mutex);
    ++state.snapshot.network_bootstraps;
    state.snapshot.explicit_endpoint_seen = explicit_server != nullptr;
    if (explicit_server != nullptr) {
      *output = *explicit_server;
    } else if (state.plan.fallback_server_available) {
      output->host = "127.0.0.1";
      output->port = 17806U;
      output->spki_sha256_base64 =
          "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=";
    } else {
      return false;
    }
  }
  state.changed.notify_all();
  return true;
}

}  // namespace boompi::network

namespace boompi::ui {

DeviceUi::~DeviceUi() noexcept { Close(); }

bool DeviceUi::Open() noexcept { return true; }

void DeviceUi::SetState(const DeviceUiState state_value) noexcept {
  auto& state = tests::State();
  std::lock_guard<std::mutex> lock(state.mutex);
  state.snapshot.ui_states.push_back(state_value);
}

void DeviceUi::SetText(const std::string_view first,
                       const std::string_view second) noexcept {
  auto& state = tests::State();
  std::lock_guard<std::mutex> lock(state.mutex);
  state.snapshot.ui_text.emplace_back(first, second);
}

bool DeviceUi::PollAction(DeviceUiAction* const action) noexcept {
  if (action == nullptr) return false;
  auto& state = tests::State();
  std::lock_guard<std::mutex> lock(state.mutex);
  if (state.actions.empty()) return false;
  *action = state.actions.front();
  state.actions.pop_front();
  return true;
}

void DeviceUi::SetBrightness(std::uint8_t) noexcept {}
void DeviceUi::Close() noexcept {}

}  // namespace boompi::ui
