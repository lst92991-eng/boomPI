#include "boompi/platform/rv1106/rockchip_voice_dsp.h"

#include <algorithm>
#include <cstdint>
#include <new>
#include <type_traits>

#include "rkaudio_preprocess.h"

namespace boompi::platform::rv1106 {
namespace {

constexpr int kSampleRateHz = 16000;
constexpr int kBitsPerSample = 16;
constexpr int kMicrophoneChannels = 2;
constexpr int kReferenceChannels = 1;
constexpr int kVendorBlockSamples = 256;
constexpr int kVendorInputShorts =
    kVendorBlockSamples * (kMicrophoneChannels + kReferenceChannels);
constexpr int kVendorOutputBytes =
    kVendorBlockSamples * static_cast<int>(sizeof(std::int16_t));
constexpr int kMainFeatureMask = RKAUDIO_EN_AEC | RKAUDIO_EN_BF;
constexpr int kBeamformingFeatureMask =
    EN_Fastaec | EN_Dereverberation | EN_AES | EN_Agc | EN_Anr |
    EN_HOWLING;

using InitSignature = void* (*)(int, int, int, int, RKAUDIOParam*);
using ProcessSignature = int (*)(void*, short*, short*, int, int*);
using DestroySignature = void (*)(void*);

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
static_assert(RKAUDIO_EN_AEC == (1 << 0), "unexpected AEC feature bit");
static_assert(RKAUDIO_EN_BF == (1 << 1), "unexpected BF feature bit");
static_assert(EN_DELAY == (1 << 0), "unexpected AEC delay-estimator bit");
static_assert(EN_Agc == (1 << 5), "unexpected AGC feature bit");
static_assert(EN_Anr == (1 << 6), "unexpected ANR feature bit");
static_assert(EN_Fix == (1 << 9), "unexpected fixed-beam feature bit");
static_assert(kBeamformingFeatureMask == 16501,
              "validated Rockchip 3A profile changed");

void ReleaseParameters(RKAUDIOParam* const parameters) noexcept {
  if (parameters != nullptr) {
    rkaudio_param_deinit(parameters);
    delete parameters;
  }
}

bool PrepareParameters(RKAUDIOParam* const parameters) noexcept {
  parameters->model_en = kMainFeatureMask;
  parameters->read_size = kVendorBlockSamples;
  parameters->aec_param = rkaudio_aec_param_init();
  parameters->bf_param = rkaudio_preprocess_param_init();
  parameters->rx_param = nullptr;
  if (parameters->aec_param == nullptr || parameters->bf_param == nullptr) {
    return false;
  }

  auto* const aec = static_cast<SKVAECParameter*>(parameters->aec_param);
  if (aec->delay_para == nullptr) {
    return false;
  }
  // Software reference alignment is the only AEC setting that differs from
  // the pinned SDK initializer.
  aec->pos = 1;
  aec->model_aec_en = EN_DELAY;
  aec->drop_ref_channel = 0;

  auto* const beamforming =
      static_cast<SKVPreprocessParam*>(parameters->bf_param);
  beamforming->model_bf_en = kBeamformingFeatureMask;
  beamforming->Targ = 4;
  beamforming->ref_pos = 1;
  beamforming->num_ref_channel = kReferenceChannels;
  beamforming->drop_ref_channel = 0;
  if (beamforming->dereverb_para == nullptr || beamforming->aes_para == nullptr ||
      beamforming->anr_para == nullptr || beamforming->agc_para == nullptr ||
      beamforming->howl_para == nullptr) {
    return false;
  }

  auto* const dereverb =
      static_cast<RKAudioDereverbParam*>(beamforming->dereverb_para);
  dereverb->curveLg = 20;
  dereverb->T60 = 0.4F;

  auto* const aes = static_cast<RKAudioAESParameter*>(beamforming->aes_para);
  aes->Beta_Up_Low = 0.005F;
  aes->THD_Flag = 0;
  aes->HARD_Flag = 0;

  auto* const anr = static_cast<SKVANRParam*>(beamforming->anr_para);
  anr->swU = 1;
  anr->fGmin = 0.01F;
  anr->InterV = 1;

  auto* const agc = static_cast<RKAGCParam*>(beamforming->agc_para);
  agc->attack_time = 200.0F;
  agc->max_gain = 25.0F;
  agc->max_peak = -1.0F;
  agc->fRth0 = -55.0F;
  agc->fRth1 = -45.0F;
  agc->fRth2 = -30.0F;
  agc->swSmL0 = 40;

  static_cast<RKHOWLParam*>(beamforming->howl_para)->howlMode = 4;
  return true;
}

}  // namespace

RockchipVoiceDsp::~RockchipVoiceDsp() noexcept {
  Close();
}

RockchipVoiceDspStatus RockchipVoiceDsp::Open() noexcept {
  if (handle_ != nullptr || parameters_ != nullptr) {
    return RockchipVoiceDspStatus::kAlreadyOpen;
  }

  auto* const parameters = new (std::nothrow) RKAUDIOParam{};
  if (parameters == nullptr) {
    return RockchipVoiceDspStatus::kParameterAllocationFailed;
  }
  if (!PrepareParameters(parameters)) {
    ReleaseParameters(parameters);
    return RockchipVoiceDspStatus::kParameterInitializationFailed;
  }

  void* const handle = rkaudio_preprocess_init(
      kSampleRateHz, kBitsPerSample, kMicrophoneChannels,
      kReferenceChannels, parameters);
  if (handle == nullptr) {
    ReleaseParameters(parameters);
    return RockchipVoiceDspStatus::kBackendInitializationFailed;
  }

  parameters_ = parameters;
  handle_ = handle;
  ResetFifos(true);
  return RockchipVoiceDspStatus::kOk;
}

void RockchipVoiceDsp::Close() noexcept {
  if (handle_ != nullptr) {
    rkaudio_preprocess_destory(handle_);
    handle_ = nullptr;
  }
  if (parameters_ != nullptr) {
    ReleaseParameters(static_cast<RKAUDIOParam*>(parameters_));
    parameters_ = nullptr;
  }
  ResetFifos(false);
}

RockchipVoiceDspStatus RockchipVoiceDsp::Process(
    const RockchipVoiceFrame16k& mic_left,
    const RockchipVoiceFrame16k& mic_right,
    const RockchipVoiceFrame16k& playback_reference,
    RockchipVoiceFrame16k* const output) noexcept {
  if (output == nullptr) {
    return RockchipVoiceDspStatus::kInvalidArgument;
  }
  output->fill(0);
  if (!is_open()) {
    return RockchipVoiceDspStatus::kNotOpen;
  }
  if (!PushInput(mic_left, mic_right, playback_reference)) {
    Close();
    return RockchipVoiceDspStatus::kFifoOverflow;
  }
  while (input_count_ >= kVendorBlockSamples) {
    if (!ProcessVendorBlock()) {
      Close();
      return RockchipVoiceDspStatus::kBackendProcessFailed;
    }
  }
  if (!PopOutput(output)) {
    Close();
    return RockchipVoiceDspStatus::kFifoUnderflow;
  }
  return RockchipVoiceDspStatus::kOk;
}

void RockchipVoiceDsp::ResetFifos(const bool prime_output) noexcept {
  input_fifo_.fill(0);
  output_fifo_.fill(0);
  vendor_input_.fill(0);
  vendor_output_.fill(0);
  input_head_ = 0U;
  input_count_ = 0U;
  output_head_ = 0U;
  output_count_ = prime_output ? kRockchipVoiceFrameSamples16k : 0U;
}

bool RockchipVoiceDsp::PushInput(
    const RockchipVoiceFrame16k& mic_left,
    const RockchipVoiceFrame16k& mic_right,
    const RockchipVoiceFrame16k& playback_reference) noexcept {
  if (input_count_ > kInputFifoFrames - kRockchipVoiceFrameSamples16k) {
    return false;
  }
  for (std::size_t sample = 0U; sample < kRockchipVoiceFrameSamples16k;
       ++sample) {
    const std::size_t frame =
        (input_head_ + input_count_ + sample) % kInputFifoFrames;
    const std::size_t base = frame * kVendorInputChannels;
    input_fifo_[base] = mic_left[sample];
    input_fifo_[base + 1U] = mic_right[sample];
    input_fifo_[base + 2U] = playback_reference[sample];
  }
  input_count_ += kRockchipVoiceFrameSamples16k;
  return true;
}

bool RockchipVoiceDsp::ProcessVendorBlock() noexcept {
  if (output_count_ > kOutputFifoSamples - kVendorBlockSamples) {
    return false;
  }
  for (std::size_t sample = 0U; sample < kVendorBlockSamples; ++sample) {
    const std::size_t frame = (input_head_ + sample) % kInputFifoFrames;
    const std::size_t source = frame * kVendorInputChannels;
    const std::size_t target = sample * kVendorInputChannels;
    std::copy_n(input_fifo_.data() + source, kVendorInputChannels,
                vendor_input_.data() + target);
  }

  int wakeup_status = 0;
  const int result = rkaudio_preprocess_short(
      handle_, reinterpret_cast<short*>(vendor_input_.data()),
      reinterpret_cast<short*>(vendor_output_.data()), kVendorInputShorts,
      &wakeup_status);
  if (result != kVendorOutputBytes) {
    return false;
  }

  input_head_ = (input_head_ + kVendorBlockSamples) % kInputFifoFrames;
  input_count_ -= kVendorBlockSamples;
  for (std::size_t sample = 0U; sample < kVendorBlockSamples; ++sample) {
    const std::size_t target =
        (output_head_ + output_count_ + sample) % kOutputFifoSamples;
    output_fifo_[target] = vendor_output_[sample];
  }
  output_count_ += kVendorBlockSamples;
  return true;
}

bool RockchipVoiceDsp::PopOutput(
    RockchipVoiceFrame16k* const output) noexcept {
  if (output == nullptr || output_count_ < kRockchipVoiceFrameSamples16k) {
    return false;
  }
  for (std::size_t sample = 0U; sample < kRockchipVoiceFrameSamples16k;
       ++sample) {
    (*output)[sample] =
        output_fifo_[(output_head_ + sample) % kOutputFifoSamples];
  }
  output_head_ =
      (output_head_ + kRockchipVoiceFrameSamples16k) % kOutputFifoSamples;
  output_count_ -= kRockchipVoiceFrameSamples16k;
  return true;
}

}  // namespace boompi::platform::rv1106
