#include "boompi/platform/rv1106/rockchip_3a_audio_dsp_adapter.h"

#include <cstdint>
#include <new>
#include <type_traits>

#include "rkaudio_preprocess.h"

namespace boompi::platform::rv1106 {
namespace {

constexpr int kSampleRateHz = 16000;
constexpr int kBitsPerSample = 16;
constexpr int kSourceChannels =
    static_cast<int>(kRockchip3aSourceChannels);
constexpr int kReferenceChannels =
    static_cast<int>(kRockchip3aReferenceChannels);
constexpr int kFrameSamplesPerChannel =
    static_cast<int>(audio::kDspBlockSamplesPerChannel16k);
constexpr int kInputSamples = static_cast<int>(kRockchip3aInputSamples);

using InitSignature = void* (*)(int, int, int, int, RKAUDIOParam*);
using ProcessSignature = int (*)(void*, short*, short*, int, int*);
using DestroySignature = void (*)(void*);
using ParamDeinitSignature = void (*)(void*);

static_assert(sizeof(short) == sizeof(std::int16_t),
              "Rockchip 3A requires a 16-bit short");
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

RKAUDIOParam* Parameters(void* const parameters) noexcept {
  return static_cast<RKAUDIOParam*>(parameters);
}

}  // namespace

Rockchip3aPreprocessSession::~Rockchip3aPreprocessSession() noexcept {
  Destroy();
}

Rockchip3aVendorInitResult
Rockchip3aPreprocessSession::Initialize() noexcept {
  if (handle_ != nullptr || parameters_ != nullptr) {
    return {Rockchip3aVendorInitCode::kAlreadyInitialized};
  }

  auto* const parameters = new (std::nothrow) RKAUDIOParam{};
  if (parameters == nullptr) {
    return {Rockchip3aVendorInitCode::kParameterAllocationFailed};
  }

  parameters->model_en = RKAUDIO_EN_AEC | RKAUDIO_EN_BF;
  parameters->aec_param = rkaudio_aec_param_init();
  parameters->bf_param = rkaudio_preprocess_param_init();
  parameters->rx_param = nullptr;
  parameters->read_size = kFrameSamplesPerChannel;
  if (parameters->aec_param == nullptr || parameters->bf_param == nullptr) {
    rkaudio_param_deinit(parameters);
    delete parameters;
    return {Rockchip3aVendorInitCode::kParameterInitializationFailed};
  }

  auto* const aec = static_cast<SKVAECParameter*>(parameters->aec_param);
  aec->pos = 1;
  aec->drop_ref_channel = 0;
  aec->model_aec_en = 0;

  auto* const beamforming =
      static_cast<SKVPreprocessParam*>(parameters->bf_param);
  beamforming->model_bf_en = EN_Fix | EN_Anr | EN_Agc;
  beamforming->Targ = 0;
  beamforming->ref_pos = 1;
  beamforming->num_ref_channel = kReferenceChannels;
  beamforming->drop_ref_channel = 0;

  void* const handle = rkaudio_preprocess_init(
      kSampleRateHz, kBitsPerSample, kSourceChannels, kReferenceChannels,
      parameters);
  if (handle == nullptr) {
    rkaudio_param_deinit(parameters);
    delete parameters;
    return {Rockchip3aVendorInitCode::kBackendInitializationFailed};
  }

  parameters_ = parameters;
  handle_ = handle;
  return {Rockchip3aVendorInitCode::kReady};
}

Rockchip3aVendorProcessResult Rockchip3aPreprocessSession::Process(
    const Rockchip3aInterleavedBlock16k& input,
    audio::DspMonoBlock16k16ms* const output) noexcept {
  if (handle_ == nullptr || parameters_ == nullptr) {
    return {Rockchip3aVendorProcessCode::kNotInitialized, 0U};
  }
  if (output == nullptr) {
    return {Rockchip3aVendorProcessCode::kInvalidArgument, 0U};
  }

  int wakeup_status = 0;
  const int result = rkaudio_preprocess_short(
      handle_, const_cast<short*>(
                   reinterpret_cast<const short*>(input.data())),
      reinterpret_cast<short*>(output->data()), kInputSamples,
      &wakeup_status);
  if (result == static_cast<int>(kRockchip3aOutputBytes)) {
    return {Rockchip3aVendorProcessCode::kProduced,
            static_cast<std::uint32_t>(result)};
  }
  if (result == 0) {
    return {Rockchip3aVendorProcessCode::kBackendRejectedInput, 0U};
  }
  return {Rockchip3aVendorProcessCode::kUnexpectedOutput, 0U};
}

void Rockchip3aPreprocessSession::Destroy() noexcept {
  if (handle_ != nullptr) {
    rkaudio_preprocess_destory(handle_);
    handle_ = nullptr;
  }
  if (parameters_ != nullptr) {
    RKAUDIOParam* const parameters = Parameters(parameters_);
    rkaudio_param_deinit(parameters);
    delete parameters;
    parameters_ = nullptr;
  }
}

}  // namespace boompi::platform::rv1106
