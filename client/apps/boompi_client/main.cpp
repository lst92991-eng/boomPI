#include <cstdlib>
#include <iostream>
#include <string_view>

#include "boompi/application/runtime.h"
#include "boompi/config/client_config.h"
#include "boompi/platform/monotonic_clock.h"
#include "boompi/platform/rv1106_build_environment.h"

namespace {

int ReportFailure(const char* const operation, const boompi::Status& status) {
  std::cerr << "boompi-client: " << operation << " failed: "
            << status.message() << '\n';
  return EXIT_FAILURE;
}

}  // namespace

int main(const int argc, char* argv[]) {
  if (argc > 2) {
    std::cerr << "usage: boompi-client [--check-config|--run-once]\n";
    return EXIT_FAILURE;
  }

  const std::string_view argument = argc == 2 ? argv[1] : "--run-once";
  if (argument != "--check-config" && argument != "--run-once") {
    std::cerr << "usage: boompi-client [--check-config|--run-once]\n";
    return EXIT_FAILURE;
  }

#if defined(BOOMPI_TARGET_RV1106)
  const auto target_status = boompi::platform::ValidateRv1106TargetBuild();
  if (!target_status.ok()) {
    return ReportFailure("RV1106 compiler target validation", target_status);
  }
#endif

  const auto config = boompi::config::DefaultClientConfig();
  const auto config_status = boompi::config::ValidateClientConfig(config);
  if (!config_status.ok()) {
    return ReportFailure("configuration validation", config_status);
  }
  if (argument == "--check-config") {
    std::cout << "boompi-client: default configuration is valid\n";
    return EXIT_SUCCESS;
  }

  boompi::platform::PosixMonotonicClock clock;
  boompi::application::Runtime runtime(config, clock);
  const auto start_status = runtime.Start();
  if (!start_status.ok()) {
    return ReportFailure("runtime start", start_status);
  }

  std::cout << "boompi-client: P1 runtime initialized\n";
  const auto stop_status = runtime.Stop();
  if (!stop_status.ok()) {
    return ReportFailure("runtime stop", stop_status);
  }
  return EXIT_SUCCESS;
}
