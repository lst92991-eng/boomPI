#include "snowboy_legacy_bridge_fake.h"

#include <cstdint>

struct BoompiSnowboyLegacyHandle final {
  std::uint32_t marker;
};

namespace {

BoompiSnowboyLegacyHandle g_handle{0x534E4F57U};
boompi::test::SnowboyLegacyBridgeFakeState g_state{};

}  // namespace

namespace boompi::test {

SnowboyLegacyBridgeFakeState& SnowboyLegacyBridgeFake() noexcept {
  return g_state;
}

void ResetSnowboyLegacyBridgeFake() noexcept {
  g_state = SnowboyLegacyBridgeFakeState{};
}

}  // namespace boompi::test

BoompiSnowboyLegacyStatus boompi_snowboy_legacy_create(
    const char* const resource_path, const char* const model_path,
    const char* const sensitivity, const float audio_gain,
    const int apply_frontend, BoompiSnowboyLegacyHandle** const handle,
    BoompiSnowboyLegacyInfo* const info) {
  (void)resource_path;
  (void)model_path;
  (void)sensitivity;
  (void)audio_gain;
  (void)apply_frontend;
  auto& state = boompi::test::SnowboyLegacyBridgeFake();
  ++state.create_calls;
  if (handle == nullptr || info == nullptr) {
    return BOOMPI_SNOWBOY_LEGACY_INVALID_ARGUMENT;
  }
  *handle = nullptr;
  *info = BoompiSnowboyLegacyInfo{};
  if (state.create_status != BOOMPI_SNOWBOY_LEGACY_OK) {
    return state.create_status;
  }
  *handle = &g_handle;
  *info = state.info;
  return BOOMPI_SNOWBOY_LEGACY_OK;
}

void boompi_snowboy_legacy_destroy(
    BoompiSnowboyLegacyHandle* const handle) {
  if (handle != nullptr) {
    ++boompi::test::SnowboyLegacyBridgeFake().destroy_calls;
  }
}

BoompiSnowboyLegacyStatus boompi_snowboy_legacy_reset(
    BoompiSnowboyLegacyHandle* const handle, int* const reset) {
  auto& state = boompi::test::SnowboyLegacyBridgeFake();
  ++state.reset_calls;
  if (handle == nullptr || reset == nullptr) {
    return BOOMPI_SNOWBOY_LEGACY_INVALID_ARGUMENT;
  }
  *reset = state.reset_result;
  return state.reset_status;
}

BoompiSnowboyLegacyStatus boompi_snowboy_legacy_process_s16(
    BoompiSnowboyLegacyHandle* const handle,
    const int16_t* const samples, const uint32_t sample_count,
    int32_t* const detection_result) {
  auto& state = boompi::test::SnowboyLegacyBridgeFake();
  ++state.process_calls;
  if (handle == nullptr || samples == nullptr || sample_count == 0U ||
      detection_result == nullptr) {
    return BOOMPI_SNOWBOY_LEGACY_INVALID_ARGUMENT;
  }
  state.last_sample_count = sample_count;
  state.first_sample = samples[0];
  state.last_sample = samples[sample_count - 1U];
  *detection_result = state.process_result;
  return state.process_status;
}
