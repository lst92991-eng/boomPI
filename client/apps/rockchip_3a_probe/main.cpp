#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <type_traits>

#include "rkaudio_preprocess.h"

namespace {

constexpr int kSampleRateHz = 16000;
constexpr int kBitsPerSample = 16;
constexpr int kSourceChannels = 2;
constexpr int kReferenceChannels = 2;
constexpr int kFrameSamplesPerChannel = 256;
constexpr int kInputSamplesTotal =
    kFrameSamplesPerChannel * (kSourceChannels + kReferenceChannels);
constexpr int kOutputBytes =
    kFrameSamplesPerChannel * static_cast<int>(sizeof(std::int16_t));
constexpr std::size_t kGuardSamples = 8U;
constexpr short kGuardValue = static_cast<short>(0x5A5A);

using InitSignature = void* (*)(int, int, int, int, RKAUDIOParam*);
using ProcessSignature = int (*)(void*, short*, short*, int, int*);
using DestroySignature = void (*)(void*);
using ParamDeinitSignature = void (*)(void*);

static_assert(sizeof(short) == sizeof(std::int16_t),
              "Rockchip 3A probe requires a 16-bit short");
static_assert(
    std::is_same<decltype(&rkaudio_preprocess_init), InitSignature>::value,
    "unexpected rkaudio_preprocess_init signature");
static_assert(
    std::is_same<decltype(&rkaudio_preprocess_short), ProcessSignature>::value,
    "unexpected rkaudio_preprocess_short signature");
static_assert(std::is_same<decltype(&rkaudio_preprocess_destory),
                           DestroySignature>::value,
              "unexpected rkaudio_preprocess_destory signature");
static_assert(
    std::is_same<decltype(&rkaudio_param_deinit), ParamDeinitSignature>::value,
    "unexpected rkaudio_param_deinit signature");
static_assert(RKAUDIO_EN_AEC == (1 << 0), "unexpected AEC feature bit");
static_assert(RKAUDIO_EN_BF == (1 << 1), "unexpected BF feature bit");
static_assert(EN_Agc == (1 << 5), "unexpected AGC feature bit");
static_assert(EN_Anr == (1 << 6), "unexpected ANR feature bit");
static_assert(EN_Fix == (1 << 9), "unexpected fixed-beam feature bit");

struct RunResult final {
  bool initialized{false};
  bool guards_intact{false};
  int invalid_input_result{0};
  int valid_result_bytes{0};
  int wakeup_status{0};
};

using GuardedOutput =
    std::array<short, kFrameSamplesPerChannel + (2U * kGuardSamples)>;

bool GuardsIntact(const GuardedOutput& output) {
  return std::all_of(
             output.begin(), output.begin() + kGuardSamples,
             [](const short sample) { return sample == kGuardValue; }) &&
         std::all_of(output.end() - kGuardSamples, output.end(),
                     [](const short sample) { return sample == kGuardValue; });
}

RunResult RunOnce() {
  RunResult result{};
  RKAUDIOParam parameters{};
  parameters.model_en = RKAUDIO_EN_AEC | RKAUDIO_EN_BF;
  parameters.aec_param = rkaudio_aec_param_init();
  parameters.bf_param = rkaudio_preprocess_param_init();
  parameters.rx_param = nullptr;
  parameters.read_size = kFrameSamplesPerChannel;

  if (parameters.aec_param == nullptr || parameters.bf_param == nullptr) {
    rkaudio_param_deinit(&parameters);
    return result;
  }

  auto* const aec = static_cast<SKVAECParameter*>(parameters.aec_param);
  aec->pos = 1;
  aec->drop_ref_channel = 0;
  aec->model_aec_en = 0;

  auto* const beamforming =
      static_cast<SKVPreprocessParam*>(parameters.bf_param);
  beamforming->model_bf_en = EN_Fix | EN_Anr | EN_Agc;
  beamforming->Targ = 0;
  beamforming->ref_pos = 1;
  beamforming->num_ref_channel = kReferenceChannels;
  beamforming->drop_ref_channel = 0;

  void* const handle =
      rkaudio_preprocess_init(kSampleRateHz, kBitsPerSample, kSourceChannels,
                              kReferenceChannels, &parameters);
  if (handle == nullptr) {
    rkaudio_param_deinit(&parameters);
    return result;
  }
  result.initialized = true;

  std::array<short, kInputSamplesTotal> input{};
  GuardedOutput guarded_output{};
  guarded_output.fill(kGuardValue);
  short* const output = guarded_output.data() + kGuardSamples;

  result.invalid_input_result =
      rkaudio_preprocess_short(handle, input.data(), output,
                               kInputSamplesTotal - 1, &result.wakeup_status);
  result.valid_result_bytes = rkaudio_preprocess_short(
      handle, input.data(), output, kInputSamplesTotal, &result.wakeup_status);
  result.guards_intact = GuardsIntact(guarded_output);

  rkaudio_preprocess_destory(handle);
  rkaudio_param_deinit(&parameters);
  return result;
}

bool RunSucceeded(const RunResult& result) {
  return result.initialized && result.guards_intact &&
         result.invalid_input_result == 0 &&
         result.valid_result_bytes == kOutputBytes;
}

}  // namespace

int main() {
  const RunResult first = RunOnce();
  const RunResult reinitialized = RunOnce();
  const bool passed = RunSucceeded(first) && RunSucceeded(reinitialized);

  std::printf("{\"schema_version\":1,\"probe\":\"rockchip_3a_offline\","
              "\"sample_rate_hz\":%d,\"bits_per_sample\":%d,"
              "\"source_channels\":%d,\"reference_channels\":%d,"
              "\"frame_samples_per_channel\":%d,\"input_samples_total\":%d,"
              "\"invalid_input_result\":%d,\"valid_result_bytes\":%d,"
              "\"wakeup_status\":%d,\"guards_intact\":%s,"
              "\"reinitialize_ok\":%s,\"status\":\"%s\"}\n",
              kSampleRateHz, kBitsPerSample, kSourceChannels,
              kReferenceChannels, kFrameSamplesPerChannel, kInputSamplesTotal,
              first.invalid_input_result, first.valid_result_bytes,
              first.wakeup_status, first.guards_intact ? "true" : "false",
              RunSucceeded(reinitialized) ? "true" : "false",
              passed ? "passed" : "failed");
  return passed ? 0 : 1;
}
