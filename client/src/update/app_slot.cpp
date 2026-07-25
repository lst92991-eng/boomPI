#include "boompi/update/app_slot.h"

namespace boompi::update {

const char* AppSlotName(const AppSlot slot) noexcept {
  return slot == AppSlot::kA ? "A" : "B";
}

AppSlot InactiveSlot(const AppSlot active_slot) noexcept {
  return active_slot == AppSlot::kA ? AppSlot::kB : AppSlot::kA;
}

Status ParseAppSlot(const std::string_view text, AppSlot* const output) {
  if (output == nullptr) {
    return Status::Error(StatusCode::kInvalidArgument,
                         "application slot output must not be null");
  }
  if (text == "A" || text == "app_A") {
    *output = AppSlot::kA;
    return Status::Ok();
  }
  if (text == "B" || text == "app_B") {
    *output = AppSlot::kB;
    return Status::Ok();
  }
  return Status::Error(StatusCode::kInvalidArgument,
                       "application slot must be A or B");
}

}  // namespace boompi::update
