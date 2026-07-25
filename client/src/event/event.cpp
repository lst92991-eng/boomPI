#include "boompi/event/event.h"

namespace boompi::event {

EventBus::EventBus(const std::size_t capacity) : capacity_(capacity) {}

Status EventBus::Publish(const Event& event) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (events_.size() >= capacity_) {
    return Status::Error(StatusCode::kResourceExhausted,
                         "event bus capacity exhausted");
  }
  events_.push_back(event);
  return Status::Ok();
}

bool EventBus::TryPop(Event* const event) {
  if (event == nullptr) {
    return false;
  }

  std::lock_guard<std::mutex> lock(mutex_);
  if (events_.empty()) {
    return false;
  }
  *event = events_.front();
  events_.pop_front();
  return true;
}

std::size_t EventBus::size() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return events_.size();
}

std::size_t EventBus::capacity() const noexcept { return capacity_; }

}  // namespace boompi::event
