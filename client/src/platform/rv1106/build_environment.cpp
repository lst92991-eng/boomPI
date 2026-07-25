#include "boompi/platform/rv1106_build_environment.h"

namespace boompi::platform {

bool IsRv1106TargetBuild() noexcept {
#if defined(BOOMPI_TARGET_RV1106) && defined(__linux__) && defined(__arm__) && \
    defined(__ARM_PCS_VFP)
  return true;
#else
  return false;
#endif
}

Status ValidateRv1106TargetBuild() {
#if !defined(BOOMPI_TARGET_RV1106)
  return Status::Error(StatusCode::kNotSupported,
                       "RV1106 CMake target marker is missing");
#elif !defined(__linux__)
  return Status::Error(StatusCode::kNotSupported,
                       "RV1106 target requires the Linux compiler macro");
#elif !defined(__arm__)
  return Status::Error(StatusCode::kNotSupported,
                       "RV1106 target requires 32-bit ARM compiler macros");
#elif !defined(__ARM_PCS_VFP)
  return Status::Error(StatusCode::kNotSupported,
                       "RV1106 target requires the ARM hard-float ABI macro");
#else
  return Status::Ok();
#endif
}

}  // namespace boompi::platform
