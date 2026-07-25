#include "boompi/event/status.h"

#include <utility>

namespace boompi {

Status::Status(const StatusCode code, std::string message)
    : code_(code), message_(std::move(message)) {}

Status Status::Ok() { return Status{}; }

Status Status::Error(const StatusCode code, std::string message) {
  if (code == StatusCode::kOk) {
    return Status{StatusCode::kInternal,
                  "an error status must not use StatusCode::kOk"};
  }
  return Status{code, std::move(message)};
}

bool Status::ok() const noexcept { return code_ == StatusCode::kOk; }

StatusCode Status::code() const noexcept { return code_; }

const std::string& Status::message() const noexcept { return message_; }

}  // namespace boompi
