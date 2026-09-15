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
