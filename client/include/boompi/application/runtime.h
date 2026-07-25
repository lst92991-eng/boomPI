#ifndef BOOMPI_APPLICATION_RUNTIME_H_
#define BOOMPI_APPLICATION_RUNTIME_H_

#include <cstdint>

#include "boompi/config/client_config.h"
#include "boompi/event/event.h"
#include "boompi/event/status.h"
#include "boompi/platform/monotonic_clock.h"

namespace boompi::application {

enum class RuntimeState : std::uint8_t {
  kStopped = 0,
  kStarting,
  kIdle,
  kStopping,
  kError,
};

// Runtime 由 application actor 单线程独占；EventBus 是跨 worker 的有界入口。
class Runtime final {
 public:
  Runtime(config::ClientConfig config, platform::MonotonicClock& clock);

  Runtime(const Runtime&) = delete;
  Runtime& operator=(const Runtime&) = delete;

  Status Start();
  Status Stop();
  RuntimeState state() const noexcept;
  bool TryPopEvent(event::Event* output);

 private:
  Status PublishLifecycleEvent(event::EventType type);

  config::ClientConfig config_;
  platform::MonotonicClock& clock_;
  event::EventBus event_bus_;
  RuntimeState state_{RuntimeState::kStopped};
  std::uint64_t next_event_sequence_{1U};
  std::uint32_t epoch_{1U};
};

}  // namespace boompi::application

#endif  // BOOMPI_APPLICATION_RUNTIME_H_
