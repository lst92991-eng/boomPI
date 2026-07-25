#include "boompi/network/reconnect_backoff.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

namespace boompi::network {

std::uint32_t ReconnectDelayMs(const std::uint32_t attempt,
                               const std::int32_t jitter_permille) noexcept {
  constexpr std::array<std::uint32_t, 6> kBackoffMs{
      {1000U, 2000U, 4000U, 8000U, 16000U, 30000U}};
  const auto index = std::min<std::size_t>(attempt, kBackoffMs.size() - 1U);
  const auto bounded_jitter = std::clamp(jitter_permille, -100, 100);
  const auto base_delay = static_cast<std::int64_t>(kBackoffMs[index]);
  const auto adjusted_delay =
      base_delay + (base_delay * static_cast<std::int64_t>(bounded_jitter)) /
                       1000;
  return static_cast<std::uint32_t>(adjusted_delay);
}

}  // namespace boompi::network
