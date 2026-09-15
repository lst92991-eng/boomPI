#include "wake.h"

#include "board_voice_profile.h"
#include "snowboy_legacy_bridge.h"

namespace boompi::wake {
namespace {
BoompiSnowboyLegacyHandle* detector{nullptr};
const char* failure{""};
}  // namespace

bool open() noexcept {
  if (detector != nullptr) {
    failure = "Snowboy is already open";
    return false;
  }
  // 资源和增益保持板级预置，真实Snowboy对象仍由旧ABI的C桥持有。
  if (!boompi_snowboy_legacy_create(audio::kSnowboyResource, audio::kSnowboyModel,
                                    audio::board::kWakeSensitivity, 1.0F, &detector)) {
    close();
    failure = "Snowboy initialization failed";
    return false;
  }
  failure = "";
  return true;
}

bool detect(const audio::VoiceFrame16k& pcm, bool* detected) noexcept {
  std::int32_t result = 0;
  if (detector == nullptr || detected == nullptr ||
      !boompi_snowboy_legacy_process_s16(detector, pcm.data(), pcm.size(), &result)) {
    failure = "Snowboy processing failed";
    return false;
  }
  *detected = result > 0;
  return true;
}

bool reset() noexcept {
  if (detector == nullptr || !boompi_snowboy_legacy_reset(detector)) {
    failure = "Snowboy reset failed";
    return false;
  }
  return true;
}
const char* error() noexcept {
  return failure;
}
void close() noexcept {
  if (detector != nullptr) {
    boompi_snowboy_legacy_destroy(detector);
    detector = nullptr;
  }
  failure = "";
}
}  // namespace boompi::wake
