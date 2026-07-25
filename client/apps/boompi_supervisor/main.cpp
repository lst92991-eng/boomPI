#include <cstdlib>
#include <iostream>
#include <string_view>

#include "boompi/update/app_slot.h"

int main(const int argc, char* argv[]) {
  if (argc != 2 || std::string_view(argv[1]) != "--check-config") {
    std::cerr << "usage: boompi-supervisor --check-config\n"
              << "process supervision is introduced in the P6 milestone\n";
    return EXIT_FAILURE;
  }

  boompi::update::AppSlot active_slot{};
  const auto parse_status = boompi::update::ParseAppSlot("A", &active_slot);
  if (!parse_status.ok() ||
      boompi::update::InactiveSlot(active_slot) != boompi::update::AppSlot::kB) {
    std::cerr << "boompi-supervisor: slot configuration is invalid\n";
    return EXIT_FAILURE;
  }

  std::cout
      << "boompi-supervisor: P1 slot parser valid; supervision not implemented\n";
  return EXIT_SUCCESS;
}
