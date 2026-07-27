#ifndef BOOMPI_TESTS_FIXTURES_SNOWBOY_LEGACY_BRIDGE_FAKE_H_
#define BOOMPI_TESTS_FIXTURES_SNOWBOY_LEGACY_BRIDGE_FAKE_H_

#include <cstdint>

#include "snowboy_legacy_bridge.h"

namespace boompi::test {

struct SnowboyLegacyBridgeFakeState final {
  BoompiSnowboyLegacyStatus create_status{BOOMPI_SNOWBOY_LEGACY_OK};
  BoompiSnowboyLegacyStatus reset_status{BOOMPI_SNOWBOY_LEGACY_OK};
  BoompiSnowboyLegacyStatus process_status{BOOMPI_SNOWBOY_LEGACY_OK};
  BoompiSnowboyLegacyInfo info{16000U, 1U, 16U, 1U};
  std::int32_t process_result{0};
  int reset_result{1};
  std::uint32_t create_calls{0U};
  std::uint32_t destroy_calls{0U};
  std::uint32_t reset_calls{0U};
  std::uint32_t process_calls{0U};
  std::uint32_t last_sample_count{0U};
  std::int16_t first_sample{0};
  std::int16_t last_sample{0};
};

SnowboyLegacyBridgeFakeState& SnowboyLegacyBridgeFake() noexcept;
void ResetSnowboyLegacyBridgeFake() noexcept;

}  // namespace boompi::test

#endif  // BOOMPI_TESTS_FIXTURES_SNOWBOY_LEGACY_BRIDGE_FAKE_H_
