#include "boompi/ui/ui_model.h"

namespace boompi::ui {

Status SetVolume(UiModel* const model, const std::int32_t volume_percent) {
  if (model == nullptr) {
    return Status::Error(StatusCode::kInvalidArgument,
                         "UI model must not be null");
  }
  if (volume_percent < 0 || volume_percent > 100) {
    return Status::Error(StatusCode::kInvalidArgument,
                         "volume must be between 0 and 100");
  }
  model->volume_percent = static_cast<std::uint8_t>(volume_percent);
  return Status::Ok();
}

Status SetBrightness(UiModel* const model,
                     const std::int32_t brightness_percent) {
  if (model == nullptr) {
    return Status::Error(StatusCode::kInvalidArgument,
                         "UI model must not be null");
  }
  if (brightness_percent < 0 || brightness_percent > 100) {
    return Status::Error(StatusCode::kInvalidArgument,
                         "brightness must be between 0 and 100");
  }
  model->brightness_percent = static_cast<std::uint8_t>(brightness_percent);
  return Status::Ok();
}

}  // namespace boompi::ui
