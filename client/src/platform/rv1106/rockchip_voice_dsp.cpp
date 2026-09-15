/**
 * @file rockchip_voice_dsp.cpp
 * @brief 对匹配 BSP 的 librkaudio 3A ABI 做固定参数、固定帧适配。
 *
 * Open 建立参数树与句柄并预置一帧静音；Process 将 320 个双麦/参考采样时刻
 * 顺序补齐 256 样本输入块，vendor 直接写输出 FIFO，再取走 320 样本。
 * PCM 与输入元数据一起延迟一帧。运行期由采集线程独占，不承担播放或网络控制。
 */
#include "boompi/platform/rv1106/rockchip_voice_dsp.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <new>
#include <type_traits>

#include "rkaudio_preprocess.h"

namespace boompi::platform::rv1106 {
namespace {

// 保留当前适配器的16kHz/双麦/单参考契约，修改支持范围需匹配SDK与真板证据。
constexpr int kSampleRateHz = 16000;
constexpr int kBitsPerSample = 16;
constexpr int kMicrophoneChannels = 2;
constexpr int kReferenceChannels = 1;
// 当前read_size配置为256，产品层以320 samples/20 ms交付；不据此断言SDK只支持256。
constexpr int kVendorBlockSamples = 256;
constexpr int kVendorInputShorts =
    kVendorBlockSamples * (kMicrophoneChannels + kReferenceChannels);
constexpr int kVendorOutputBytes = kVendorBlockSamples * static_cast<int>(sizeof(std::int16_t));
constexpr int kMainFeatureMask = RKAUDIO_EN_AEC | RKAUDIO_EN_BF;
// BF 参数树承载 FastAEC、残余回声抑制、降噪、去混响和双讲保护的组合开关。
constexpr int kBeamformingFeatureMask =
    EN_Fastaec | EN_AES | EN_Anr | EN_Dereverberation | EN_STDT;

using InitSignature = void* (*)(int, int, int, int, RKAUDIOParam*);
using ProcessSignature = int (*)(void*, short*, short*, int, int*);
using DestroySignature = void (*)(void*);

// 编译期核对 vendor 头文件 ABI 和已验证 feature 数值，BSP 变更会在构建时明确失败。
static_assert(sizeof(short) == sizeof(std::int16_t), "Rockchip 3A requires a 16-bit short");
static_assert(std::is_same<decltype(&rkaudio_preprocess_init), InitSignature>::value,
              "unexpected rkaudio_preprocess_init signature");
static_assert(std::is_same<decltype(&rkaudio_preprocess_short), ProcessSignature>::value,
              "unexpected rkaudio_preprocess_short signature");
static_assert(std::is_same<decltype(&rkaudio_preprocess_destory), DestroySignature>::value,
              "unexpected rkaudio_preprocess_destory signature");
static_assert(kBeamformingFeatureMask == 1109, "validated Rockchip 3A profile changed");
/** @brief 释放完整或部分初始化的参数树；Open 的失败回滚与 Close 共用。 */
void ReleaseParameters(RKAUDIOParam* const parameters) noexcept {
  // vendor deinit 释放各子参数树，外层 RKAUDIOParam 由本适配器负责 delete。
  if (parameters == nullptr) {
    return;
  }
  rkaudio_param_deinit(parameters);
  delete parameters;
}

/**
 * @brief 根据维护者 profile 建立当前 BSP 的 AEC、波束成形和降噪参数。
 * 调用方传入零初始化的参数树；任一必要子对象缺失返回 false，外层统一回滚。
 * feature mask 未启用 vendor AGC，避免与音频硬件或用户增益形成多重自动增益。
 */
bool PrepareParameters(RKAUDIOParam* const parameters, const int delay_samples) noexcept {
  // 参数树只在 Open 构造一次；实时 Process 仅使用已验证句柄，不进行配置或分配。
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
  // Mode1 的 mic/reference 位于同一采集 period，硬回采不启用软件延迟估计。
  aec->pos = 1;
  aec->model_aec_en = 0;
  aec->drop_ref_channel = 0;
  aec->delay_len = delay_samples;

  auto* const beamforming = static_cast<SKVPreprocessParam*>(parameters->bf_param);
  beamforming->model_bf_en = kBeamformingFeatureMask;
  beamforming->Targ = 4;
  beamforming->ref_pos = 1;
  beamforming->num_ref_channel = kReferenceChannels;
  beamforming->drop_ref_channel = 0;
  // 本产品实际启用的模块都必须拿到 vendor 默认参数对象，缺失任一对象都停止初始化。
  if (beamforming->dereverb_para == nullptr || beamforming->aes_para == nullptr ||
      beamforming->anr_para == nullptr || beamforming->dtd_para == nullptr) {
    return false;
  }

  // STDT 只在 vendor 内部保护双讲；公开 ABI 没有可供 application 读取的 DTD 事件。
  auto* const dtd = static_cast<RKDTDParam*>(beamforming->dtd_para);
  dtd->ksiThd_high = 0.70F;
  dtd->ksiThd_low = 0.50F;

  auto* const dereverb = static_cast<RKAudioDereverbParam*>(beamforming->dereverb_para);
  dereverb->curveLg = 20;
  dereverb->T60 = 0.4F;

  auto* const aes = static_cast<RKAudioAESParameter*>(beamforming->aes_para);
  aes->Beta_Up_Low = 0.005F;
  aes->THD_Flag = 0;
  aes->HARD_Flag = 0;
  auto* const anr = static_cast<SKVANRParam*>(beamforming->anr_para);
  // 保留经板端回归验证的温和降噪，避免把近讲辅音当成噪声强行压掉。
  anr->swU = 1;
  anr->fGmin = 0.01F;
  anr->InterV = 1;

  return true;
}

}  // namespace

RockchipVoiceDsp::~RockchipVoiceDsp() noexcept {
  Close();
}

bool RockchipVoiceDsp::Open(const int delay_samples) noexcept {
  // 重复 Open 通常代表所有权错误，保留现有资源并让调用方显式处理失败。
  if (handle_ != nullptr || parameters_ != nullptr || delay_samples < 0 ||
      delay_samples % kVendorBlockSamples != 0) {
    return false;
  }

  auto* const parameters = new (std::nothrow) RKAUDIOParam{};
  if (parameters == nullptr) {
    return false;
  }
  if (!PrepareParameters(parameters, delay_samples)) {
    ReleaseParameters(parameters);
    return false;
  }

  void* const handle = rkaudio_preprocess_init(
      kSampleRateHz, kBitsPerSample, kMicrophoneChannels, kReferenceChannels, parameters);
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

bool RockchipVoiceDsp::Process(const audio::CaptureChannels& input,
                               audio::CleanAudioFrame* const output) noexcept {
  if (output == nullptr) {
    return false;
  }
  *output = {};
  if (!IsOpen()) {
    return false;
  }
  // 跨帧余数留在同一个256样本块中；每个输入样本只复制一次，不搬移整帧FIFO。
  for (std::size_t i = 0U; i < kRockchipVoiceFrameSamples16k; ++i) {
    const std::size_t base = input_count_ * kVendorInputChannels;
    input_block_[base] = input.mic_left[i];
    input_block_[base + 1U] = input.mic_right[i];
    input_block_[base + 2U] = input.reference_left[i];
    if (++input_count_ != kVendorBlockSamples) {
      continue;
    }
    // 先检查剩余容量，再让vendor直接写入，避免独立中转数组与第二次复制。
    if (output_count_ > kOutputFifoSamples - kVendorBlockSamples) {
      Close();
      return false;
    }
    int wakeup_status = 0;
    const int result =
        rkaudio_preprocess_short(handle_, reinterpret_cast<short*>(input_block_.data()),
                                 reinterpret_cast<short*>(output_fifo_.data() + output_count_),
                                 kVendorInputShorts, &wakeup_status);
    if (result != kVendorOutputBytes) {
      // vendor 返回长度或 FIFO 不变量失效后，继续处理会错位整个音频时间轴。
      Close();
      return false;
    }
    input_count_ = 0U;
    output_count_ += kVendorBlockSamples;
  }
  if (output_count_ < output->pcm.size()) {
    Close();
    return false;
  }
  // 每次对外稳定取出 320 samples；prime 的静音使输入、输出始终保持固定 20 ms 延迟。
  std::copy_n(output_fifo_.data(), output->pcm.size(), output->pcm.data());
  output_count_ -= output->pcm.size();
  std::memmove(output_fifo_.data(), output_fifo_.data() + output->pcm.size(),
               output_count_ * sizeof(std::int16_t));
  output->metadata = previous_metadata_;
  if (output->metadata.timestamp_us == 0U) {
    // 首次输出是预置静音，没有上一帧时刻；用当前观测时刻占位，电平/参考仍保持静音初值。
    output->metadata.timestamp_us = input.metadata.timestamp_us;
  }
  previous_metadata_ = input.metadata;
  return true;
}

void RockchipVoiceDsp::ResetFifos(const bool prime_output) noexcept {
  input_block_.fill(0);
  output_fifo_.fill(0);
  input_count_ = 0U;
  output_count_ = prime_output ? kRockchipVoiceFrameSamples16k : 0U;
  previous_metadata_ = {};
}

}  // namespace boompi::platform::rv1106
