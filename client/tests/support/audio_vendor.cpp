#include "audio_vendor.h"

#include <cstddef>
#include <cstdint>
#include <new>

#include "rkaudio_preprocess.h"

namespace boompi::test::audio_vendor {
std::atomic<int> vad_result{1}, wake_result{0};
std::atomic<bool> snowboy_process_ok{true};
std::atomic<unsigned> dsp_calls{0}, dsp_failure_call{0};
void reset() noexcept {
  vad_result = 1;
  wake_result = 0;
  snowboy_process_ok = true;
  dsp_calls = dsp_failure_call = 0;
}
}  // namespace boompi::test::audio_vendor

using namespace boompi::test::audio_vendor;
struct VadInst {};
struct BoompiSnowboyLegacyHandle {};
extern "C" {
void* rkaudio_aec_param_init() {
  static SKVAECParameter parameters{};
  parameters.delay_para = &parameters;
  return &parameters;
}
void* rkaudio_preprocess_param_init() {
  static RKAudioDereverbParam dereverb{};
  static RKAudioAESParameter aes{};
  static SKVANRParam anr{};
  static RKDTDParam dtd{};
  static SKVPreprocessParam parameters{0, 0, 0, 0, 0, &dereverb, &aes, &anr, &dtd};
  return &parameters;
}
void rkaudio_param_deinit(RKAUDIOParam*) {}
void* rkaudio_preprocess_init(int rate, int bits, int microphones, int references,
                              RKAUDIOParam* parameters) {
  dsp_calls = 0;
  return rate == 16000 && bits == 16 && microphones == 2 && references == 1 ? parameters
                                                                            : nullptr;
}
int rkaudio_preprocess_short(void* handle, short* input, short* output, int count, int*) {
  ++dsp_calls;
  if (handle == nullptr || count != 768 || dsp_calls == dsp_failure_call) {
    return 0;
  }
  for (int i = 0; i < 256; ++i) {
    output[i] = static_cast<short>(input[3 * i] + 2 * input[3 * i + 1] - input[3 * i + 2]);
  }
  return 512;
}
void rkaudio_preprocess_destory(void*) {}
VadInst* WebRtcVad_Create() {
  return new (std::nothrow) VadInst;
}
void WebRtcVad_Free(VadInst* vad) {
  delete vad;
}
int WebRtcVad_Init(VadInst* vad) {
  return vad == nullptr ? -1 : 0;
}
int WebRtcVad_set_mode(VadInst* vad, int mode) {
  return vad != nullptr && mode == 3 ? 0 : -1;
}
int WebRtcVad_ValidRateAndFrameLength(int rate, std::size_t samples) {
  return rate == 16000 && samples == 320 ? 0 : -1;
}
int WebRtcVad_Process(VadInst* vad, int rate, const std::int16_t* pcm, std::size_t samples) {
  return vad != nullptr && pcm != nullptr &&
                 WebRtcVad_ValidRateAndFrameLength(rate, samples) == 0
             ? vad_result.load()
             : -1;
}
int boompi_snowboy_legacy_create(const char*, const char*, const char*, float,
                                 BoompiSnowboyLegacyHandle** handle) {
  *handle = new (std::nothrow) BoompiSnowboyLegacyHandle;
  return *handle != nullptr;
}
void boompi_snowboy_legacy_destroy(BoompiSnowboyLegacyHandle* handle) {
  delete handle;
}
int boompi_snowboy_legacy_reset(BoompiSnowboyLegacyHandle* handle) {
  return handle != nullptr;
}
int boompi_snowboy_legacy_process_s16(BoompiSnowboyLegacyHandle* handle,
                                      const std::int16_t* pcm, std::uint32_t samples,
                                      std::int32_t* result) {
  if (handle == nullptr || pcm == nullptr || samples != 320 || result == nullptr ||
      !snowboy_process_ok) {
    return 0;
  }
  *result = wake_result;
  return 1;
}
}
