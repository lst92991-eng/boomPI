#include "boompi/platform/monotonic_clock.h"

#include <chrono>

namespace boompi::platform {

std::uint64_t PosixMonotonicClock::NowMs() const noexcept {
  const auto now = std::chrono::steady_clock::now().time_since_epoch();
  const auto milliseconds =
      std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
  return static_cast<std::uint64_t>(milliseconds);
}

}  // namespace boompi::platform
