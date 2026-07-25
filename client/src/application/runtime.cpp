#include "boompi/application/runtime.h"

#include <utility>

namespace boompi::application {

Runtime::Runtime(config::ClientConfig config, platform::MonotonicClock& clock)
    : config_(std::move(config)),
      clock_(clock),
      event_bus_(config_.event_queue_capacity) {}

Status Runtime::Start() {
  if (state_ != RuntimeState::kStopped) {
    return Status::Error(StatusCode::kFailedPrecondition,
                         "runtime can only start from the stopped state");
  }

  state_ = RuntimeState::kStarting;
  const auto config_status = config::ValidateClientConfig(config_);
  if (!config_status.ok()) {
    state_ = RuntimeState::kError;
    return config_status;
  }

  const auto publish_status =
      PublishLifecycleEvent(event::EventType::kRuntimeStarted);
  if (!publish_status.ok()) {
    state_ = RuntimeState::kError;
    return publish_status;
  }
  state_ = RuntimeState::kIdle;
  return Status::Ok();
}

Status Runtime::Stop() {
  if (state_ == RuntimeState::kStopped) {
    return Status::Ok();
  }
  if (state_ == RuntimeState::kStarting || state_ == RuntimeState::kStopping) {
    return Status::Error(StatusCode::kFailedPrecondition,
                         "runtime transition is already in progress");
  }

  state_ = RuntimeState::kStopping;
  const auto publish_status =
      PublishLifecycleEvent(event::EventType::kRuntimeStopping);
  if (!publish_status.ok()) {
    state_ = RuntimeState::kError;
    return publish_status;
  }
  state_ = RuntimeState::kStopped;
  return Status::Ok();
}

RuntimeState Runtime::state() const noexcept { return state_; }

bool Runtime::TryPopEvent(event::Event* const output) {
  return event_bus_.TryPop(output);
}

Status Runtime::PublishLifecycleEvent(const event::EventType type) {
  event::Event lifecycle_event{};
  lifecycle_event.type = type;
  lifecycle_event.monotonic_timestamp_ms = clock_.NowMs();
  lifecycle_event.sequence = next_event_sequence_++;
  lifecycle_event.epoch = epoch_;
  lifecycle_event.turn_id = 0U;
  return event_bus_.Publish(lifecycle_event);
}

}  // namespace boompi::application
