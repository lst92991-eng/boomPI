#ifndef BOOMPI_NETWORK_RECONNECT_BACKOFF_H_
#define BOOMPI_NETWORK_RECONNECT_BACKOFF_H_

#include <cstdint>

namespace boompi::network {

// jitter_permille is clamped to +/-100, yielding at most +/-10 percent jitter.
std::uint32_t ReconnectDelayMs(std::uint32_t attempt,
                               std::int32_t jitter_permille) noexcept;

}  // namespace boompi::network

#endif  // BOOMPI_NETWORK_RECONNECT_BACKOFF_H_
