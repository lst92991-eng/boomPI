#include "boompi/platform/rv1106/webrtc_vad_gate.h"

#include <algorithm>
#include <cstddef>

extern "C" {
#include "webrtc_vad.h"
}

namespace boompi::platform::rv1106 {
namespace {

constexpr std::int32_t kSampleRateHz = 16000;
constexpr std::uint16_t kFrameDurationMs = 20U;
constexpr std::size_t kFrameSamples = 320U;

VadInst* ToVad(void* const handle) noexcept {
  return static_cast<VadInst*>(handle);
}

}  // namespace

WebRtcVadGate::~WebRtcVadGate() noexcept {
  if (vad_handle_ != nullptr) {
    WebRtcVad_Free(ToVad(vad_handle_));
  }
  vad_handle_ = nullptr;
}

bool WebRtcVadGate::Open(const WebRtcVadConfig& config) noexcept {
  if (vad_handle_ != nullptr) {
    WebRtcVad_Free(ToVad(vad_handle_));
  }
  vad_handle_ = nullptr;
  if (config.mode < 0 || config.mode > 3 || config.speech_start_ms == 0U ||
      config.speech_end_ms == 0U ||
      WebRtcVad_ValidRateAndFrameLength(kSampleRateHz, kFrameSamples) != 0) {
    return false;
  }

  VadInst* const vad = WebRtcVad_Create();
  if (vad == nullptr || WebRtcVad_Init(vad) != 0 ||
      WebRtcVad_set_mode(vad, config.mode) != 0) {
    if (vad != nullptr) {
      WebRtcVad_Free(vad);
    }
    return false;
  }
  vad_handle_ = vad;
  config_ = config;
  Reset();
  return true;
}

void WebRtcVadGate::Reset() noexcept {
  speech_ms_ = 0U;
  silence_ms_ = 0U;
  in_speech_ = false;
}

WebRtcVadUpdate WebRtcVadGate::Process(
    const audio::Mono16kFrame& frame) noexcept {
  WebRtcVadUpdate update{};
  if (vad_handle_ == nullptr || !frame.HasValidLength()) {
    return update;
  }
  const int result = WebRtcVad_Process(ToVad(vad_handle_), kSampleRateHz,
                                       frame.samples.data(), kFrameSamples);
  if (result < 0) {
    return update;
  }

  update.valid = true;
  update.speech_now = result == 1;
  if (update.speech_now) {
    speech_ms_ = std::min<std::uint32_t>(
        speech_ms_ + kFrameDurationMs, config_.speech_start_ms);
    silence_ms_ = 0U;
  } else {
    silence_ms_ = std::min<std::uint32_t>(
        silence_ms_ + kFrameDurationMs, config_.speech_end_ms);
    speech_ms_ = 0U;
  }

  if (!in_speech_ && speech_ms_ >= config_.speech_start_ms) {
    in_speech_ = true;
    update.speech_started = true;
  } else if (in_speech_ && silence_ms_ >= config_.speech_end_ms) {
    in_speech_ = false;
    update.speech_ended = true;
  }
  return update;
}

}  // namespace boompi::platform::rv1106
