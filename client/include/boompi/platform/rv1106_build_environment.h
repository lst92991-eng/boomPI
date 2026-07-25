#ifndef BOOMPI_PLATFORM_RV1106_BUILD_ENVIRONMENT_H_
#define BOOMPI_PLATFORM_RV1106_BUILD_ENVIRONMENT_H_

#include "boompi/event/status.h"

namespace boompi::platform {

bool IsRv1106TargetBuild() noexcept;
Status ValidateRv1106TargetBuild();

}  // namespace boompi::platform

#endif  // BOOMPI_PLATFORM_RV1106_BUILD_ENVIRONMENT_H_
