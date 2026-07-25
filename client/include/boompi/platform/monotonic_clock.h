#ifndef BOOMPI_PLATFORM_MONOTONIC_CLOCK_H_
#define BOOMPI_PLATFORM_MONOTONIC_CLOCK_H_

#include <cstdint>

namespace boompi::platform {

class MonotonicClock {
 public:
  virtual ~MonotonicClock() = default;
  virtual std::uint64_t NowMs() const noexcept = 0;
};

class PosixMonotonicClock final : public MonotonicClock {
 public:
  std::uint64_t NowMs() const noexcept override;
};

}  // namespace boompi::platform

#endif  // BOOMPI_PLATFORM_MONOTONIC_CLOCK_H_
