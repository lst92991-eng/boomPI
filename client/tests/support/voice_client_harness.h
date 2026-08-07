#pragma once

#include <csignal>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "boompi/audio/audio_engine.h"
#include "boompi/network/voice_transport.h"
#include "boompi/ui/device_ui.h"

namespace boompi::tests {

struct CaptureStep final {
  audio::CaptureFrame frame{};
  std::vector<network::Inbound> inbound;
  std::vector<ui::DeviceUiAction> actions;
  bool sequence_gap_before{false};
  bool playback_done{false};
  bool playback_failed{false};
  bool timeout{false};
  unsigned delay_ms{0U};
  bool stop_after{false};
};

struct HarnessPlan final {
  std::vector<CaptureStep> capture;
  std::vector<network::SendOutcome> pcm_outcomes;
  unsigned reconnect_pause_ms{1050U};
  bool fallback_server_available{true};
  bool acknowledge_hello{true};
  bool stop_during_capture_timeout{false};
};

struct PcmSend final {
  network::Pcm64Header header{};
  std::vector<std::uint8_t> payload;
};

struct HarnessSnapshot final {
  std::vector<network::Control> controls;
  std::vector<PcmSend> pcm;
  std::vector<ui::DeviceUiState> ui_states;
  std::vector<std::pair<std::string, std::string>> ui_text;
  std::vector<float> playback_scales;
  std::size_t network_bootstraps{0U};
  std::size_t connections{0U};
  std::size_t transport_closes{0U};
  bool explicit_endpoint_seen{false};
  std::string connected_host;
  std::uint16_t connected_port{0U};
  std::string connected_spki;
  std::size_t listener_resets{0U};
  std::size_t playback_begins{0U};
  std::size_t playback_ends{0U};
  std::size_t playback_drops{0U};
  std::size_t capture_timeouts{0U};
  std::size_t capture_poll_wait_ms{0U};
};

/// 安装一段确定性输入脚本。stop 的生命周期必须覆盖 RunVoiceClient。
void LoadHarness(HarnessPlan plan,
                 volatile std::sig_atomic_t* stop);
HarnessSnapshot ReadHarness();

}  // namespace boompi::tests
