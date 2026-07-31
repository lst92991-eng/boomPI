#ifndef BOOMPI_ROCKCHIP_3A_HIL_PINSET_SHA256
#error "BOOMPI_ROCKCHIP_3A_HIL_PINSET_SHA256 must be provided by CMake"
#endif

#ifndef BOOMPI_ROCKCHIP_3A_HIL_SOURCE_SHA256
#error "BOOMPI_ROCKCHIP_3A_HIL_SOURCE_SHA256 must be provided by CMake"
#endif

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <sys/resource.h>

#include "rkaudio_preprocess.h"

namespace {

using Clock = std::chrono::steady_clock;

constexpr int kRateHz = 16000;
constexpr int kSampleBits = 16;
constexpr int kSourceChannels = 2;
constexpr int kReferenceChannels = 1;
constexpr int kTotalChannels = kSourceChannels + kReferenceChannels;
constexpr int kSamplesPerChannel = 256;
constexpr int kInputShorts = kSamplesPerChannel * kTotalChannels;
constexpr int kInputBytes = kInputShorts * static_cast<int>(sizeof(short));
constexpr int kOutputShorts = kSamplesPerChannel;
constexpr int kExpectedOutputBytes =
    kOutputShorts * static_cast<int>(sizeof(short));
constexpr int kMainFeatureMask = RKAUDIO_EN_AEC | RKAUDIO_EN_BF;
constexpr int kBeamformingFeatureMask =
    EN_Fastaec | EN_Dereverberation | EN_AES | EN_Agc | EN_Anr |
    EN_HOWLING;
#ifdef BOOMPI_ROCKCHIP_3A_HIL_TEST_VENDOR_LIBRARY_PATH
constexpr const char* kVendorLibraryPath =
    BOOMPI_ROCKCHIP_3A_HIL_TEST_VENDOR_LIBRARY_PATH;
#else
constexpr const char* kVendorLibraryPath =
    "/oem/usr/lib/libaec_bf_process.so";
#endif
constexpr std::uint32_t kInputGuardBefore = UINT32_C(0x713A2C5D);
constexpr std::uint32_t kInputGuardAfter = UINT32_C(0x91B4E607);
constexpr std::uint32_t kOutputGuardBefore = UINT32_C(0x4D28A17C);
constexpr std::uint32_t kOutputGuardAfter = UINT32_C(0xE63B5092);

static_assert(sizeof(short) == 2, "Rockchip 3A requires 16-bit short");
static_assert(kInputShorts == 768, "fixed 2-mic plus reference input changed");
static_assert(kInputBytes == 1536, "fixed input byte count changed");
static_assert(kExpectedOutputBytes == 512,
              "fixed mono output byte count changed");
static_assert(kBeamformingFeatureMask == 16501,
              "fixed feasibility feature profile changed");

struct GuardedInput {
  std::uint32_t before = kInputGuardBefore;
  std::array<short, static_cast<std::size_t>(kInputShorts)> samples {};
  std::uint32_t after = kInputGuardAfter;
};

struct GuardedOutput {
  std::uint32_t before = kOutputGuardBefore;
  std::array<short, static_cast<std::size_t>(kOutputShorts)> samples {};
  std::uint32_t after = kOutputGuardAfter;
};

struct Options {
  bool help = false;
  bool execute = false;
  bool allow_vendor_call = false;
};

struct ExecutionReport {
  bool vendor_library_load_attempted = false;
  bool vendor_library_loaded = false;
  bool vendor_symbols_resolved = false;
  bool vendor_library_unloaded = false;
  bool parameter_preparation_passed = false;
  bool init_attempted = false;
  bool init_returned_handle = false;
  bool process_attempted = false;
  int process_return = 0;
  int wakeup_status_raw = 0;
  bool destroy_called = false;
  bool parameter_deinit_called = false;
  bool guards_evaluated = false;
  bool input_guards_intact = false;
  bool output_guards_intact = false;
  std::int64_t init_elapsed_us = 0;
  std::int64_t process_elapsed_us = 0;
  std::int64_t destroy_elapsed_us = 0;
  const char* probe_status = "fail";
};

struct VendorApi {
  void* library = nullptr;
  decltype(&rkaudio_preprocess_init) preprocess_init = nullptr;
  decltype(&rkaudio_preprocess_short) preprocess_short = nullptr;
  decltype(&rkaudio_preprocess_destory) preprocess_destory = nullptr;
};

const char* JsonBool(bool value) { return value ? "true" : "false"; }

std::int64_t ElapsedMicroseconds(Clock::time_point start,
                                 Clock::time_point end) {
  return std::chrono::duration_cast<std::chrono::microseconds>(end - start)
      .count();
}

void PrintUsage(const char* program) {
  std::printf(
      "Usage:\n"
      "  %s\n"
      "  %s --execute --allow-rockchip-3a-call\n\n"
      "No arguments performs a dry-run and invokes no Rockchip 3A API.\n"
      "Execution processes exactly one fixed in-memory synthetic frame; it "
      "does not open PCM, read an audio file, or write an artifact.\n",
      program, program);
}

bool ParseOptions(int argc, char** argv, Options* options) {
  if (argc == 2 && std::strcmp(argv[1], "--help") == 0) {
    options->help = true;
    return true;
  }
  for (int index = 1; index < argc; ++index) {
    if (std::strcmp(argv[index], "--execute") == 0 && !options->execute) {
      options->execute = true;
    } else if (std::strcmp(argv[index], "--allow-rockchip-3a-call") == 0 &&
               !options->allow_vendor_call) {
      options->allow_vendor_call = true;
    } else {
      return false;
    }
  }
  return options->execute == options->allow_vendor_call;
}

bool LoaderOverrideEnvironmentAbsent() {
  static constexpr const char* kLoaderOverrideVariables[] = {
      "LD_LIBRARY_PATH", "LD_PRELOAD", "LD_AUDIT"};
  for (const char* variable : kLoaderOverrideVariables) {
    if (std::getenv(variable) != nullptr) {
      std::fprintf(stderr,
                   "rockchip_3a_hil: loader override environment is set\n");
      return false;
    }
  }
  return true;
}

bool EstablishExecutionSafetyPreconditions() {
  static constexpr const char* kVendorDebugPathVariables[] = {
      "PATH_RX_IN",       "PATH_RX_OUT",      "PATH_TX_IN_MIC",
      "PATH_TX_IN_REF",   "PATH_TX_OUT_MDF", "PATH_TX_OUT_MDFFAST",
  };
  for (const char* variable : kVendorDebugPathVariables) {
    if (std::getenv(variable) != nullptr) {
      std::fprintf(stderr,
                   "rockchip_3a_hil: vendor debug-path environment is set\n");
      return false;
    }
  }

  const rlimit core_limit = {0, 0};
  if (setrlimit(RLIMIT_CORE, &core_limit) != 0) {
    std::fprintf(stderr,
                 "rockchip_3a_hil: could not disable process core dumps\n");
    return false;
  }
  return true;
}

template <typename FunctionPointer>
bool LoadVendorSymbol(void* library, const char* name,
                      FunctionPointer* output) {
  static_assert(sizeof(FunctionPointer) == sizeof(void*),
                "target function pointers must match dlsym handles");
  dlerror();
  void* symbol = dlsym(library, name);
  if (dlerror() != nullptr || symbol == nullptr) {
    return false;
  }
  std::memcpy(output, &symbol, sizeof(symbol));
  return true;
}

bool LoadVendorApi(VendorApi* vendor) {
  vendor->library = dlopen(kVendorLibraryPath, RTLD_NOW | RTLD_LOCAL);
  if (vendor->library == nullptr) {
    std::fprintf(stderr, "rockchip_3a_hil: vendor library load failed\n");
    return false;
  }
  const bool loaded =
      LoadVendorSymbol(vendor->library, "rkaudio_preprocess_init",
                       &vendor->preprocess_init) &&
      LoadVendorSymbol(vendor->library, "rkaudio_preprocess_short",
                       &vendor->preprocess_short) &&
      LoadVendorSymbol(vendor->library, "rkaudio_preprocess_destory",
                       &vendor->preprocess_destory);
  if (!loaded) {
    std::fprintf(stderr, "rockchip_3a_hil: vendor symbol resolution failed\n");
  }
  return loaded;
}

bool UnloadVendorApi(VendorApi* vendor) {
  if (vendor->library == nullptr) {
    return true;
  }
  const bool unloaded = dlclose(vendor->library) == 0;
  *vendor = VendorApi {};
  return unloaded;
}

void PrintContract() {
  std::printf(
      "    \"rate_hz\": %d,\n"
      "    \"sample_bits\": %d,\n"
      "    \"samples_per_channel\": %d,\n"
      "    \"source_channels\": %d,\n"
      "    \"reference_channels\": %d,\n"
      "    \"channel_order\": \"mic0,mic1,reference\",\n"
      "    \"input_shorts\": %d,\n"
      "    \"input_bytes\": %d,\n"
      "    \"input_size_argument_shorts\": %d,\n"
      "    \"output_capacity_shorts\": %d,\n"
      "    \"expected_output_bytes\": %d,\n"
      "    \"main_feature_mask\": %d,\n"
      "    \"beamforming_feature_mask\": %d,\n"
      "    \"configuration_file_loaded\": false,\n"
      "    \"input_kind\": \"bounded_low_amplitude_synthetic\"\n",
      kRateHz, kSampleBits, kSamplesPerChannel, kSourceChannels,
      kReferenceChannels, kInputShorts, kInputBytes, kInputShorts,
      kOutputShorts, kExpectedOutputBytes, kMainFeatureMask,
      kBeamformingFeatureMask);
}

void PrintDryRun() {
  std::printf("{\n");
  std::printf("  \"schema_version\": 1,\n");
  std::printf("  \"probe\": \"rockchip_3a_fixed_frame\",\n");
  std::printf("  \"mode\": \"dry_run\",\n");
  std::printf("  \"provenance\": {\n");
  std::printf("    \"pinset_sha256\": \"%s\",\n",
              BOOMPI_ROCKCHIP_3A_HIL_PINSET_SHA256);
  std::printf("    \"source_sha256\": \"%s\"\n",
              BOOMPI_ROCKCHIP_3A_HIL_SOURCE_SHA256);
  std::printf("  },\n");
  std::printf("  \"contract\": {\n");
  PrintContract();
  std::printf("  },\n");
  std::printf("  \"vendor_calls_attempted\": false,\n");
  std::printf("  \"vendor_library_load_attempted\": false,\n");
  std::printf("  \"mutated_external_state\": \"not_evaluated\",\n");
  std::printf("  \"probe_status\": \"dry_run\",\n");
  std::printf("  \"full_hil_status\": \"not_evaluated\",\n");
  std::printf("  \"physical_layout_verified\": false,\n");
  std::printf("  \"aec_effect_verified\": false,\n");
  std::printf("  \"realtime_rate_verified\": false\n");
  std::printf("}\n");
}

void PrintExecution(const ExecutionReport& report) {
  std::printf("{\n");
  std::printf("  \"schema_version\": 1,\n");
  std::printf("  \"probe\": \"rockchip_3a_fixed_frame\",\n");
  std::printf("  \"mode\": \"execute\",\n");
  std::printf("  \"provenance\": {\n");
  std::printf("    \"pinset_sha256\": \"%s\",\n",
              BOOMPI_ROCKCHIP_3A_HIL_PINSET_SHA256);
  std::printf("    \"source_sha256\": \"%s\"\n",
              BOOMPI_ROCKCHIP_3A_HIL_SOURCE_SHA256);
  std::printf("  },\n");
  std::printf("  \"contract\": {\n");
  PrintContract();
  std::printf("  },\n");
  std::printf(
      "  \"vendor_library\": {\"path\": \"%s\", "
      "\"load_attempted\": %s, \"loaded\": %s, "
      "\"symbols_resolved\": %s, \"unloaded\": %s},\n",
      kVendorLibraryPath, JsonBool(report.vendor_library_load_attempted),
      JsonBool(report.vendor_library_loaded),
      JsonBool(report.vendor_symbols_resolved),
      JsonBool(report.vendor_library_unloaded));
  std::printf("  \"parameter_preparation_passed\": %s,\n",
              JsonBool(report.parameter_preparation_passed));
  std::printf("  \"calls\": {\n");
  std::printf(
      "    \"init\": {\"attempted\": %s, \"returned_handle\": %s, "
      "\"elapsed_us\": %lld},\n",
      JsonBool(report.init_attempted), JsonBool(report.init_returned_handle),
      static_cast<long long>(report.init_elapsed_us));
  std::printf(
      "    \"process\": {\"attempted\": %s, \"return_value\": %d, "
      "\"elapsed_us\": %lld},\n",
      JsonBool(report.process_attempted), report.process_return,
      static_cast<long long>(report.process_elapsed_us));
  std::printf(
      "    \"destroy\": {\"called\": %s, \"elapsed_us\": %lld},\n",
      JsonBool(report.destroy_called),
      static_cast<long long>(report.destroy_elapsed_us));
  std::printf("    \"parameter_deinit\": {\"called\": %s}\n",
              JsonBool(report.parameter_deinit_called));
  std::printf("  },\n");
  std::printf("  \"wakeup_status_raw\": %d,\n",
              report.wakeup_status_raw);
  std::printf("  \"wakeup_status_is_product_vad\": false,\n");
  std::printf("  \"guards\": {\"evaluated\": %s, "
              "\"input_intact\": %s, \"output_intact\": %s},\n",
              JsonBool(report.guards_evaluated),
              JsonBool(report.input_guards_intact),
              JsonBool(report.output_guards_intact));
  std::printf("  \"mutated_external_state\": \"not_evaluated\",\n");
  std::printf("  \"probe_status\": \"%s\",\n", report.probe_status);
  std::printf("  \"full_hil_status\": \"not_evaluated\",\n");
  std::printf("  \"physical_layout_verified\": false,\n");
  std::printf("  \"aec_effect_verified\": false,\n");
  std::printf("  \"realtime_rate_verified\": false\n");
  std::printf("}\n");
}

bool PrepareParameters(RKAUDIOParam* parameters) {
  static constexpr float kLimitRatio[2][3] = {
      {2.0F, 1.5F, 1.0F},
      {1.5F, 1.2F, 1.0F},
  };
  static constexpr short kThdSplitFreq[4][2] = {
      {0, 0}, {1500, 2000}, {2000, 6000}, {6000, 8000}};
  static constexpr float kThdSupDegree[4][10] = {
      {0.01F, 0.01F, 0.005F, 0.005F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F,
       0.0F},
      {0.0005F, 0.0005F, 0.0005F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F,
       0.0F, 0.0F},
      {0.001F, 0.001F, 0.001F, 0.001F, 0.001F, 0.001F, 0.0F, 0.0F,
       0.0F, 0.0F},
      {0.001F, 0.001F, 0.001F, 0.001F, 0.001F, 0.001F, 0.0F, 0.0F,
       0.0F, 0.0F},
  };
  static constexpr short kHardSplitFreq[5][2] = {
      {100, 500}, {0, 0}, {0, 0}, {0, 0}, {500, 3000}};
  static constexpr float kHardThreshold[4] = {0.35F, 0.15F, 0.35F,
                                               0.15F};

  std::memset(parameters, 0, sizeof(*parameters));
  parameters->model_en = kMainFeatureMask;
  parameters->read_size = kSamplesPerChannel;
  parameters->aec_param = rkaudio_aec_param_init();
  if (parameters->aec_param == nullptr) {
    return false;
  }
  parameters->bf_param = rkaudio_preprocess_param_init();
  if (parameters->bf_param == nullptr) {
    return false;
  }

  auto* aec = static_cast<SKVAECParameter*>(parameters->aec_param);
  if (aec->delay_para == nullptr) {
    return false;
  }
  aec->pos = 1;
  aec->model_aec_en = EN_DELAY;
  aec->drop_ref_channel = 0;
  aec->delay_len = 0;
  aec->look_ahead = 0;
  aec->filter_len = 2;

  auto* beamforming =
      static_cast<SKVPreprocessParam*>(parameters->bf_param);
  beamforming->model_bf_en = kBeamformingFeatureMask;
  beamforming->Targ = 4;
  beamforming->ref_pos = 1;
  beamforming->num_ref_channel = kReferenceChannels;
  beamforming->drop_ref_channel = 0;
  if (beamforming->dereverb_para == nullptr ||
      beamforming->aes_para == nullptr || beamforming->anr_para == nullptr ||
      beamforming->agc_para == nullptr || beamforming->howl_para == nullptr) {
    return false;
  }

  // The direct API does not parse config_aivqe.json. Keep this bounded probe's
  // enabled profile byte-for-byte reviewable in source and tie it to the
  // pinned config through the CMake provenance hash.
  auto* dereverb =
      static_cast<RKAudioDereverbParam*>(beamforming->dereverb_para);
  dereverb->rlsLg = 4;
  dereverb->curveLg = 20;
  dereverb->delay = 2;
  dereverb->forgetting = 0.98F;
  dereverb->T60 = 0.4F;
  dereverb->coCoeff = 1.0F;

  auto* aes = static_cast<RKAudioAESParameter*>(beamforming->aes_para);
  aes->Beta_Up = 0.002F;
  aes->Beta_Down = 0.001F;
  aes->Beta_Up_Low = 0.005F;
  aes->Beta_Down_Low = 0.001F;
  aes->low_freq = 500;
  aes->high_freq = 3750;
  aes->THD_Flag = 0;
  aes->HARD_Flag = 0;
  std::memcpy(aes->LimitRatio, kLimitRatio, sizeof(kLimitRatio));
  std::memcpy(aes->ThdSplitFreq, kThdSplitFreq, sizeof(kThdSplitFreq));
  std::memcpy(aes->ThdSupDegree, kThdSupDegree, sizeof(kThdSupDegree));
  std::memcpy(aes->HardSplitFreq, kHardSplitFreq, sizeof(kHardSplitFreq));
  std::memcpy(aes->HardThreshold, kHardThreshold, sizeof(kHardThreshold));

  auto* anr = static_cast<SKVANRParam*>(beamforming->anr_para);
  anr->noiseFactor = 0.88F;
  anr->swU = 1;
  anr->PsiMin = 0.02F;
  anr->PsiMax = 0.516F;
  anr->fGmin = 0.01F;
  anr->Sup_Freq1 = -3588;
  anr->Sup_Freq2 = -3588;
  anr->Sup_Energy1 = 10000.0F;
  anr->Sup_Energy2 = 10000.0F;
  anr->InterV = 1;
  anr->BiasMin = 1.67F;
  anr->UpdateFrm = 15;
  anr->NPreGammaThr = 4.6F;
  anr->NPreZetaThr = 1.67F;
  anr->SabsGammaThr0 = 1.0F;
  anr->SabsGammaThr1 = 3.0F;
  anr->InfSmooth = 0.8F;
  anr->ProbSmooth = 0.7F;
  anr->CompCoeff = 1.4F;
  anr->PrioriMin = 0.0316F;
  anr->PostMax = 40.0F;
  anr->PrioriRatio = 0.95F;
  anr->PrioriRatioLow = 0.95F;
  anr->SplitBand = 20;
  anr->PrioriSmooth = 0.7F;
  anr->TranMode = 0;

  auto* agc = static_cast<RKAGCParam*>(beamforming->agc_para);
  agc->attack_time = 200.0F;
  agc->release_time = 200.0F;
  agc->attenuate_time = 1000.0F;
  agc->max_gain = 25.0F;
  agc->max_peak = -1.0F;
  agc->fRth0 = -55.0F;
  agc->fRth1 = -45.0F;
  agc->fRth2 = -30.0F;
  agc->fRk0 = 2.0F;
  agc->fRk1 = 0.8F;
  agc->fRk2 = 0.4F;
  agc->fLineGainDb = -25.0F;
  agc->swSmL0 = 40;
  agc->swSmL1 = 80;
  agc->swSmL2 = 80;
  agc->fs = kRateHz;
  agc->frmlen = kSamplesPerChannel;

  static_cast<RKHOWLParam*>(beamforming->howl_para)->howlMode = 4;
  return true;
}

void FillSyntheticInput(GuardedInput* input) {
  for (int frame = 0; frame < kSamplesPerChannel; ++frame) {
    const int phase = frame % 32;
    const std::size_t base =
        static_cast<std::size_t>(frame * kTotalChannels);
    input->samples[base] = static_cast<short>((phase - 16) * 4);
    input->samples[base + 1] =
        static_cast<short>((((phase + 5) % 32) - 16) * 3);
    input->samples[base + 2] =
        static_cast<short>((((phase + 11) % 16) - 8) * 2);
  }
}

bool InputGuardsIntact(const GuardedInput& input) {
  return input.before == kInputGuardBefore && input.after == kInputGuardAfter;
}

bool OutputGuardsIntact(const GuardedOutput& output) {
  return output.before == kOutputGuardBefore &&
         output.after == kOutputGuardAfter;
}

int ExecuteProbe() {
  ExecutionReport report;
  VendorApi vendor;
  report.vendor_library_load_attempted = true;
  report.vendor_symbols_resolved = LoadVendorApi(&vendor);
  report.vendor_library_loaded = vendor.library != nullptr;
  if (!report.vendor_symbols_resolved) {
    if (report.vendor_library_loaded) {
      report.vendor_library_unloaded = UnloadVendorApi(&vendor);
    }
    report.probe_status = "vendor_load_failed";
    PrintExecution(report);
    return 4;
  }

  RKAUDIOParam parameters {};
  report.parameter_preparation_passed = PrepareParameters(&parameters);
  if (!report.parameter_preparation_passed) {
    rkaudio_param_deinit(&parameters);
    report.parameter_deinit_called = true;
    report.vendor_library_unloaded = UnloadVendorApi(&vendor);
    report.probe_status = "parameter_preparation_failed";
    PrintExecution(report);
    return 4;
  }

  GuardedInput input;
  GuardedOutput output;
  FillSyntheticInput(&input);

  report.init_attempted = true;
  const Clock::time_point init_start = Clock::now();
  void* handle = vendor.preprocess_init(
      kRateHz, kSampleBits, kSourceChannels, kReferenceChannels, &parameters);
  const Clock::time_point init_end = Clock::now();
  report.init_elapsed_us = ElapsedMicroseconds(init_start, init_end);
  report.init_returned_handle = handle != nullptr;
  if (handle == nullptr) {
    rkaudio_param_deinit(&parameters);
    report.parameter_deinit_called = true;
    report.vendor_library_unloaded = UnloadVendorApi(&vendor);
    report.probe_status = "init_failed";
    PrintExecution(report);
    return 4;
  }

  report.process_attempted = true;
  const Clock::time_point process_start = Clock::now();
  report.process_return = vendor.preprocess_short(
      handle, input.samples.data(), output.samples.data(), kInputShorts,
      &report.wakeup_status_raw);
  const Clock::time_point process_end = Clock::now();
  report.process_elapsed_us = ElapsedMicroseconds(process_start, process_end);
  report.guards_evaluated = true;
  report.input_guards_intact = InputGuardsIntact(input);
  report.output_guards_intact = OutputGuardsIntact(output);

  const Clock::time_point destroy_start = Clock::now();
  vendor.preprocess_destory(handle);
  const Clock::time_point destroy_end = Clock::now();
  report.destroy_elapsed_us = ElapsedMicroseconds(destroy_start, destroy_end);
  report.destroy_called = true;

  // The vendor guide and wrapper both destroy the handle before releasing the
  // caller-owned parameter tree. Both APIs return void, so the report says
  // only that each call occurred; it does not invent a cleanup success code.
  rkaudio_param_deinit(&parameters);
  report.parameter_deinit_called = true;
  report.vendor_library_unloaded = UnloadVendorApi(&vendor);

  if (!report.vendor_library_unloaded) {
    report.probe_status = "vendor_unload_failed";
    PrintExecution(report);
    return 5;
  }

  if (report.process_return != kExpectedOutputBytes) {
    report.probe_status = "process_contract_failed";
    PrintExecution(report);
    return 5;
  }
  if (!report.input_guards_intact || !report.output_guards_intact) {
    report.probe_status = "memory_guard_failed";
    PrintExecution(report);
    return 5;
  }

  report.probe_status = "pass";
  PrintExecution(report);
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  Options options;
  if (!ParseOptions(argc, argv, &options)) {
    PrintUsage(argv[0]);
    return 2;
  }
  // This is a residual check, not a pre-exec barrier: a dynamic loader may
  // already have processed LD_PRELOAD/LD_AUDIT before main. The documented
  // outer `env -u` invocation is therefore mandatory for every mode.
  if (!LoaderOverrideEnvironmentAbsent()) {
    return 3;
  }
  if (options.help) {
    PrintUsage(argv[0]);
    return 0;
  }
  if (!options.execute) {
    PrintDryRun();
    return 0;
  }
  if (!EstablishExecutionSafetyPreconditions()) {
    return 3;
  }
  return ExecuteProbe();
}
