/** @file vad.cpp
 * @brief WebRTC VAD 薄接口：配置模式 3，逐块返回错误/无人声/有人声。
 * 开口确认、静音累计和插话均由 speech.cpp 处理，不能把 VAD 命中等同于一句话。
 */
#include "vad.h"
extern "C" {
#include <webrtc_vad.h>
}

namespace boompi::vad {
namespace {
VadInst* detector{};
}

bool reset() noexcept {
  return detector && WebRtcVad_Init(detector) == 0 && WebRtcVad_set_mode(detector, 3) == 0;
}

bool open() noexcept {
  if (detector) {
    return false;
  }
  detector = WebRtcVad_Create();
  if (!reset() || WebRtcVad_ValidRateAndFrameLength(16000, audio::kVoiceFrameSamples) != 0) {
    close();
    return false;
  }
  return true;
}

int process(const audio::VoiceFrame16k& pcm) noexcept {
  return detector ? WebRtcVad_Process(detector, 16000, pcm.data(), pcm.size()) : -1;
}

void close() noexcept {
  if (detector) {
    WebRtcVad_Free(detector);
    detector = nullptr;
  }
}
}  // namespace boompi::vad
