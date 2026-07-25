#ifndef BOOMPI_EVENT_EVENT_H_
#define BOOMPI_EVENT_EVENT_H_

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>

#include "boompi/event/status.h"

namespace boompi::event {

enum class EventType : std::uint8_t {
  kRuntimeStarted = 0,
  kRuntimeStopping,
  kRuntimeError,
};

struct Event final {
  EventType type{EventType::kRuntimeError};
  std::uint64_t monotonic_timestamp_ms{0};
  std::uint64_t sequence{0};
  std::uint32_t epoch{0};
  std::uint32_t turn_id{0};
};

// This mutex/deque bus is for non-real-time control threads only. Capture,
// DSP, and playback real-time threads must hand events off through a separate
// preallocated real-time queue and must never call EventBus directly.
class EventBus final {
 public:
  explicit EventBus(std::size_t capacity);

  EventBus(const EventBus&) = delete;
  EventBus& operator=(const EventBus&) = delete;

  Status Publish(const Event& event);
  bool TryPop(Event* event);
  std::size_t size() const;
  std::size_t capacity() const noexcept;

 private:
  const std::size_t capacity_;
  mutable std::mutex mutex_;
  std::deque<Event> events_;
};

}  // namespace boompi::event

#endif  // BOOMPI_EVENT_EVENT_H_
