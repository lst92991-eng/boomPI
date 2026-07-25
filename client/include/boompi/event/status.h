#ifndef BOOMPI_EVENT_STATUS_H_
#define BOOMPI_EVENT_STATUS_H_

#include <cstdint>
#include <string>

namespace boompi {

enum class StatusCode : std::uint8_t {
  kOk = 0,
  kInvalidArgument,
  kFailedPrecondition,
  kResourceExhausted,
  kNotSupported,
  kInternal,
};

class Status final {
 public:
  Status() = default;

  static Status Ok();
  static Status Error(StatusCode code, std::string message);

  bool ok() const noexcept;
  StatusCode code() const noexcept;
  const std::string& message() const noexcept;

 private:
  Status(StatusCode code, std::string message);

  StatusCode code_{StatusCode::kOk};
  std::string message_;
};

}  // namespace boompi

#endif  // BOOMPI_EVENT_STATUS_H_
