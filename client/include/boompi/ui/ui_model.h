#ifndef BOOMPI_UI_UI_MODEL_H_
#define BOOMPI_UI_UI_MODEL_H_

#include <cstdint>
#include <string>

#include "boompi/event/status.h"

namespace boompi::ui {

enum class UiState : std::uint8_t {
  kStarting = 0,
  kIdle,
  kListening,
  kThinking,
  kSpeaking,
  kOffline,
  kError,
};

struct UiModel final {
  UiState state{UiState::kStarting};
  std::uint8_t volume_percent{60};
  std::uint8_t brightness_percent{100};
  std::string user_subtitle;
  std::string assistant_subtitle;
};

Status SetVolume(UiModel* model, std::int32_t volume_percent);
Status SetBrightness(UiModel* model, std::int32_t brightness_percent);

}  // namespace boompi::ui

#endif  // BOOMPI_UI_UI_MODEL_H_
