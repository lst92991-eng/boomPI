#ifndef BOOMPI_TEST_MANUAL_CLOCK_H_
#define BOOMPI_TEST_MANUAL_CLOCK_H_

#include <cstdint>

#include "boompi/platform/monotonic_clock.h"

namespace boompi::test {

class ManualClock final : public platform::MonotonicClock {
 public:
  explicit ManualClock(std::uint64_t now_ms) : now_ms_(now_ms) {}

  std::uint64_t NowMs() const noexcept override { return now_ms_; }
  void SetNowMs(const std::uint64_t now_ms) noexcept { now_ms_ = now_ms; }

 private:
  std::uint64_t now_ms_;
};

}  // namespace boompi::test

#endif  // BOOMPI_TEST_MANUAL_CLOCK_H_
