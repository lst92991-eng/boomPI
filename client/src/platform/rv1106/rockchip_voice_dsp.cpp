/// @file 对匹配 BSP 的 librkaudio 3A ABI 做固定参数、固定帧适配。
#include "boompi/platform/rv1106/rockchip_voice_dsp.h"

#ifndef BOOMPI_ROCKCHIP_ENABLE_STDT
#define BOOMPI_ROCKCHIP_ENABLE_STDT 1
#endif
#ifndef BOOMPI_ROCKCHIP_DELAY_SAMPLES
#define BOOMPI_ROCKCHIP_DELAY_SAMPLES 0
#endif
#include <algorithm>
#include <cstring>
#include <cstdint>
#include <new>
#include <type_traits>

#include "rkaudio_preprocess.h"

namespace boompi::platform::rv1106 {
namespace {

constexpr int kSampleRateHz = 16000;
constexpr int kBitsPerSample = 16;
constexpr int kMicrophoneChannels = 2;
constexpr int kReferenceChannels = BOOMPI_ROCKCHIP_REFERENCE_CHANNELS;
constexpr int kVendorBlockSamples = 256;
constexpr int kVendorInputShorts =
    kVendorBlockSamples * (kMicrophoneChannels + kReferenceChannels);
constexpr int kVendorOutputBytes =
    kVendorBlockSamples * static_cast<int>(sizeof(std::int16_t));
constexpr int kMainFeatureMask = RKAUDIO_EN_AEC | RKAUDIO_EN_BF;
constexpr int kBeamformingFeatureMask =
    EN_Fastaec | EN_AES | EN_Anr | EN_Dereverberation |
    (BOOMPI_ROCKCHIP_ENABLE_STDT != 0 ? EN_STDT : 0);

using InitSignature = void* (*)(int, int, int, int, RKAUDIOParam*);
using ProcessSignature = int (*)(void*, short*, short*, int, int*);
using DestroySignature = void (*)(void*);

static_assert(sizeof(short) == sizeof(std::int16_t), "Rockchip 3A requires a 16-bit short");
static_assert(std::is_same<decltype(&rkaudio_preprocess_init), InitSignature>::value,
              "unexpected rkaudio_preprocess_init signature");
static_assert(std::is_same<decltype(&rkaudio_preprocess_short), ProcessSignature>::value,
              "unexpected rkaudio_preprocess_short signature");
static_assert(std::is_same<decltype(&rkaudio_preprocess_destory), DestroySignature>::value,
              "unexpected rkaudio_preprocess_destory signature");
static_assert(kBeamformingFeatureMask ==
                  (BOOMPI_ROCKCHIP_ENABLE_STDT != 0 ? 1109 : 85),
              "validated Rockchip 3A profile changed");
static_assert(BOOMPI_ROCKCHIP_DELAY_SAMPLES >= 0 &&
                  BOOMPI_ROCKCHIP_DELAY_SAMPLES % 256 == 0,
              "Rockchip fixed AEC delay must be a non-negative 256-sample multiple");

void ReleaseParameters(RKAUDIOParam* const parameters) noexcept {
  if (parameters == nullptr) return;
  rkaudio_param_deinit(parameters); delete parameters;
}

bool PrepareParameters(RKAUDIOParam* const parameters) noexcept {
  parameters->model_en = kMainFeatureMask;
  parameters->read_size = kVendorBlockSamples;
  parameters->aec_param = rkaudio_aec_param_init();
  parameters->bf_param = rkaudio_preprocess_param_init();
  parameters->rx_param = nullptr;
  if (parameters->aec_param == nullptr || parameters->bf_param == nullptr) return false;

  auto* const aec = static_cast<SKVAECParameter*>(parameters->aec_param);
  if (aec->delay_para == nullptr) return false;
  // Mode1 的 mic/reference 位于同一采集 period，硬回采不启用软件延迟估计。
  aec->pos = 1;
  aec->model_aec_en = 0;
  aec->drop_ref_channel = 0;
  aec->delay_len = BOOMPI_ROCKCHIP_DELAY_SAMPLES;

  auto* const beamforming =
      static_cast<SKVPreprocessParam*>(parameters->bf_param);
  beamforming->model_bf_en = kBeamformingFeatureMask;
  beamforming->Targ = 4;
  beamforming->ref_pos = 1;
  beamforming->num_ref_channel = kReferenceChannels;
  beamforming->drop_ref_channel = 0;
  if (beamforming->dereverb_para == nullptr || beamforming->aes_para == nullptr ||
      beamforming->anr_para == nullptr || beamforming->dtd_para == nullptr) {
    return false;
  }

  // STDT 只在 vendor 内部保护双讲；公开 ABI 没有可供 application 读取的 DTD 事件。
  auto* const dtd = static_cast<RKDTDParam*>(beamforming->dtd_para);
  dtd->ksiThd_high = 0.70F;
  dtd->ksiThd_low = 0.50F;

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

  return true;
}

}  // namespace

RockchipVoiceDsp::~RockchipVoiceDsp() noexcept {
  Close();
}

bool RockchipVoiceDsp::Open() noexcept {
  if (handle_ != nullptr || parameters_ != nullptr) return false;

  auto* const parameters = new (std::nothrow) RKAUDIOParam{};
  if (parameters == nullptr) return false;
  if (!PrepareParameters(parameters)) {
    ReleaseParameters(parameters);
    return false;
  }

  void* const handle = rkaudio_preprocess_init(
      kSampleRateHz, kBitsPerSample, kMicrophoneChannels,
      kReferenceChannels, parameters);
  if (handle == nullptr) {
    ReleaseParameters(parameters);
    return false;
  }

  parameters_ = parameters;
  handle_ = handle;
  // vendor 256-sample block 与产品 320-sample frame 不整除；prime 一帧输出让每次 API
  // 调用仍保持严格 320 in / 320 out，代价是固定 20 ms 启动延迟。
  ResetFifos(true);
  return true;
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

bool RockchipVoiceDsp::Process(
    const RockchipVoiceFrame16k& mic_left,
    const RockchipVoiceFrame16k& mic_right,
    const RockchipVoiceFrame16k& reference_left,
    const RockchipVoiceFrame16k& reference_right,
    RockchipVoiceFrame16k* const output) noexcept {
  if (output == nullptr) return false;
  output->fill(0);
  if (!is_open()) return false;
  if (input_count_ > kInputFifoFrames - kRockchipVoiceFrameSamples16k) return false;
  // Mode1 仍采集 REF-R；生产单参考只在送入 vendor 前丢弃它。
  for (std::size_t i = 0U; i < kRockchipVoiceFrameSamples16k; ++i) {
    const std::size_t base = (input_count_ + i) * kVendorInputChannels;
    input_fifo_[base] = mic_left[i]; input_fifo_[base + 1U] = mic_right[i];
    input_fifo_[base + 2U] = reference_left[i];
    if constexpr (kVendorInputChannels == 4U) input_fifo_[base + 3U] = reference_right[i];
  }
  input_count_ += kRockchipVoiceFrameSamples16k;
  while (input_count_ >= kVendorBlockSamples) {
    int wakeup_status = 0;
    const int result = rkaudio_preprocess_short(
        handle_, reinterpret_cast<short*>(input_fifo_.data()),
        reinterpret_cast<short*>(vendor_output_.data()), kVendorInputShorts,
        &wakeup_status);
    if (result != kVendorOutputBytes ||
        output_count_ > kOutputFifoSamples - kVendorBlockSamples) {
      Close(); return false;
    }
    input_count_ -= kVendorBlockSamples;
    std::memmove(input_fifo_.data(), input_fifo_.data() +
                     kVendorBlockSamples * kVendorInputChannels,
                 input_count_ * kVendorInputChannels * sizeof(std::int16_t));
    std::copy_n(vendor_output_.data(), kVendorBlockSamples,
                output_fifo_.data() + output_count_);
    output_count_ += kVendorBlockSamples;
  }
  if (output_count_ < output->size()) { Close(); return false; }
  std::copy_n(output_fifo_.data(), output->size(), output->data());
  output_count_ -= output->size();
  std::memmove(output_fifo_.data(), output_fifo_.data() + output->size(),
               output_count_ * sizeof(std::int16_t));
  return true;
}

void RockchipVoiceDsp::ResetFifos(const bool prime_output) noexcept {
  input_fifo_.fill(0);
  output_fifo_.fill(0);
  vendor_output_.fill(0);
  input_count_ = 0U;
  output_count_ = prime_output ? kRockchipVoiceFrameSamples16k : 0U;
}

}  // namespace boompi::platform::rv1106
