#ifndef BOOMPI_PLATFORM_RV1106_WEBRTC_VAD_GATE_H_
#define BOOMPI_PLATFORM_RV1106_WEBRTC_VAD_GATE_H_

#include <cstdint>

#include "boompi/audio/audio_frame.h"

namespace boompi::platform::rv1106 {

struct WebRtcVadConfig final {
  std::int32_t mode{2};
  std::uint16_t speech_start_ms{120U};
  std::uint16_t speech_end_ms{700U};
};

struct WebRtcVadUpdate final {
  bool valid{false};
  bool speech_now{false};
  bool speech_started{false};
  bool speech_ended{false};
};

// One wake/VAD worker owns this gate. Open and destruction are cold-path;
// Process is allocation-free and accepts exactly one 16 kHz, 20 ms mono frame.
class WebRtcVadGate final {
 public:
  WebRtcVadGate() noexcept = default;
  WebRtcVadGate(const WebRtcVadGate&) = delete;
  WebRtcVadGate& operator=(const WebRtcVadGate&) = delete;
  ~WebRtcVadGate() noexcept;

  bool Open(const WebRtcVadConfig& config = {}) noexcept;
  void Reset() noexcept;
  WebRtcVadUpdate Process(const audio::Mono16kFrame& frame) noexcept;

 private:
  void* vad_handle_{nullptr};
  WebRtcVadConfig config_{};
  std::uint32_t speech_ms_{0U};
  std::uint32_t silence_ms_{0U};
  bool in_speech_{false};
};

}  // namespace boompi::platform::rv1106

#endif  // BOOMPI_PLATFORM_RV1106_WEBRTC_VAD_GATE_H_
