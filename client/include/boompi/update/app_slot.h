#ifndef BOOMPI_UPDATE_APP_SLOT_H_
#define BOOMPI_UPDATE_APP_SLOT_H_

#include <string_view>

#include "boompi/event/status.h"

namespace boompi::update {

enum class AppSlot {
  kA = 0,
  kB,
};

const char* AppSlotName(AppSlot slot) noexcept;
AppSlot InactiveSlot(AppSlot active_slot) noexcept;
Status ParseAppSlot(std::string_view text, AppSlot* output);

}  // namespace boompi::update

#endif  // BOOMPI_UPDATE_APP_SLOT_H_
