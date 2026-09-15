#include "boompi/platform/rv1106/rockchip_3a.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <new>
#include <type_traits>

#include "rkaudio_preprocess.h"

namespace boompi::rockchip_3a {
namespace {

// 保留当前适配器的16kHz/双麦/单参考契约，修改支持范围需匹配SDK与真板证据。
// 当前read_size配置为256，产品层以320 samples/20 ms交付；不据此断言SDK只支持256。
constexpr int kVendorBlockSamples = 256;
constexpr std::size_t kVendorInputChannels = 3U;
constexpr std::size_t kOutputFifoSamples = 640U;
void* handle{nullptr};
RKAUDIOParam* parameters{nullptr};
std::array<std::int16_t, kVendorBlockSamples * kVendorInputChannels> input_block{};
std::array<std::int16_t, kOutputFifoSamples> output_fifo{};
std::size_t input_count{0U}, output_count{0U};
audio::CaptureMetadata previous_metadata{};

void ResetFifos(bool prime_output) noexcept {
  output_fifo.fill(0);
  input_count = 0U;
  output_count = prime_output ? audio::kVoiceFrameSamples : 0U;
  previous_metadata = {};
}
constexpr int kVendorInputShorts = kVendorBlockSamples * kVendorInputChannels;
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
}  // namespace

bool open(int delay_samples) noexcept {
  if (handle || parameters || delay_samples < 0 || delay_samples % kVendorBlockSamples != 0) {
    return false;
  }
  parameters = new (std::nothrow) RKAUDIOParam{};
  if (!parameters) {
    return false;
  }
  parameters->model_en = kMainFeatureMask;
  parameters->read_size = kVendorBlockSamples;
  parameters->aec_param = rkaudio_aec_param_init();
  parameters->bf_param = rkaudio_preprocess_param_init();
  auto* aec = static_cast<SKVAECParameter*>(parameters->aec_param);
  auto* bf = static_cast<SKVPreprocessParam*>(parameters->bf_param);
  if (!aec || !bf || !aec->delay_para || !bf->aes_para || !bf->anr_para || !bf->dereverb_para ||
      !bf->dtd_para) {
    close();
    return false;
  }
  // AEC：双麦、单数字回采；不是软件参考，不重新猜测延迟。
  aec->pos = 1;
  aec->model_aec_en = 0;
  aec->drop_ref_channel = 0;
  aec->delay_len = delay_samples;
  bf->model_bf_en = kBeamformingFeatureMask;
  bf->Targ = 4;
  bf->ref_pos = 1;
  bf->num_ref_channel = 1;
  bf->drop_ref_channel = 0;
  // AES：残余回声抑制。
  auto* aes = static_cast<RKAudioAESParameter*>(bf->aes_para);
  aes->Beta_Up_Low = 0.005F;
  aes->THD_Flag = 0;
  aes->HARD_Flag = 0;
  // ANR：沿用当前维护者预置，不启用叠加AGC。
  auto* anr = static_cast<SKVANRParam*>(bf->anr_para);
  anr->swU = 1;
  anr->fGmin = 0.01F;
  anr->InterV = 1;
  auto* dereverb = static_cast<RKAudioDereverbParam*>(bf->dereverb_para);
  dereverb->curveLg = 20;
  dereverb->T60 = 0.4F;
  auto* dtd = static_cast<RKDTDParam*>(bf->dtd_para);
  dtd->ksiThd_high = 0.70F;
  dtd->ksiThd_low = 0.50F;
  // 上面是配置阅读顺序，不代表闭源库的执行顺序。
  handle = rkaudio_preprocess_init(16000, 16, 2, 1, parameters);
  if (!handle) {
    close();
    return false;
  }
  ResetFifos(true);
  return true;
}

void close() noexcept {
  if (handle) {
    rkaudio_preprocess_destory(handle);
    handle = nullptr;
  }
  if (parameters) {
    rkaudio_param_deinit(parameters);
    delete parameters;
    parameters = nullptr;
  }
  ResetFifos(false);
}

bool process(const audio::CaptureChannels& input, audio::VoiceFrame16k* const pcm,
             audio::CaptureMetadata* const metadata) noexcept {
  if (pcm == nullptr || metadata == nullptr) {
    return false;
  }
  *pcm = {};
  *metadata = {};
  if (!handle) {
    return false;
  }
  // 跨帧余数留在同一个256样本块中；每个输入样本只复制一次，不搬移整帧FIFO。
  for (std::size_t i = 0U; i < audio::kVoiceFrameSamples; ++i) {
    const std::size_t base = input_count * kVendorInputChannels;
    input_block[base] = input.mic_left[i];
    input_block[base + 1U] = input.mic_right[i];
    input_block[base + 2U] = input.reference_left[i];
    if (++input_count != kVendorBlockSamples) {
      continue;
    }
    // 先检查剩余容量，再让vendor直接写入，避免独立中转数组与第二次复制。
    if (output_count > kOutputFifoSamples - kVendorBlockSamples) {
      close();
      return false;
    }
    int wakeup_status = 0;
    const int result =
        rkaudio_preprocess_short(handle, reinterpret_cast<short*>(input_block.data()),
                                 reinterpret_cast<short*>(output_fifo.data() + output_count),
                                 kVendorInputShorts, &wakeup_status);
    if (result != kVendorOutputBytes) {
      // vendor 返回长度或 FIFO 不变量失效后，继续处理会错位整个音频时间轴。
      close();
      return false;
    }
    input_count = 0U;
    output_count += kVendorBlockSamples;
  }
  if (output_count < pcm->size()) {
    close();
    return false;
  }
  // 每次对外稳定取出 320 samples；prime 的静音使输入、输出始终保持固定 20 ms 延迟。
  std::copy_n(output_fifo.data(), pcm->size(), pcm->data());
  output_count -= pcm->size();
  std::memmove(output_fifo.data(), output_fifo.data() + pcm->size(),
               output_count * sizeof(std::int16_t));
  *metadata = previous_metadata;
  if (metadata->timestamp_us == 0U) {
    // 首次输出是预置静音，没有上一帧时刻；用当前观测时刻占位，电平/参考仍保持静音初值。
    metadata->timestamp_us = input.metadata.timestamp_us;
  }
  previous_metadata = input.metadata;
  return true;
}

}  // namespace boompi::rockchip_3a
