/** @file audio_pipeline_test.cpp
 * @brief 同一个audio-engine harness直接验证生产转换与检测模块。
 *
 * 重采样使用Host真实FFmpeg。WebRTC/Snowboy结果和Rockchip处理内核由窄C替身控制，
 * 真实的FIFO、元数据对齐和检测逻辑保持不变；不验证厂商ABI或声学算法效果。
 *
 * 从 audio_engine_harness_test.cpp 的 pipeline-format/detection 入口进入；先验证
 * 帧格式和延迟，再用已知分类序列检查开口、收尾、播放参考等待与插话候选的时序。
 */
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <new>
#include <vector>

#include "audio_converter.h"
#include "boompi/audio/audio_tasks.h"
#include "boompi/platform/rv1106/rockchip_voice_dsp.h"
#include "rkaudio_preprocess.h"
#include "speech_detector.h"

namespace {

using namespace boompi::audio;
using boompi::platform::rv1106::AudioConverter;
using boompi::platform::rv1106::PlaybackEnd;
using boompi::platform::rv1106::PlaybackState;
using boompi::platform::rv1106::SpeechDetector;

// 这些旋钮只控制库边界的返回结果；测试不能直接修改生产模块的内部计数与状态。
int vad_result = 1;
int wake_result = 0;
bool snowboy_process_ok = true;
unsigned dsp_calls = 0U, dsp_failure_call = 0U;

/** @brief 报告单个数据链契约失败，返回结果供当前场景决定是否继续。 */
bool Check(bool condition, const char* message) {
  if (!condition) {
    std::fprintf(stderr, "audio-pipeline: %s\n", message);
  }
  return condition;
}

/// 只比较三个声学平面，元数据由对应场景另行核对。
bool EqualChannels(const CaptureChannels& left, const CaptureChannels& right) {
  return left.mic_left == right.mic_left && left.mic_right == right.mic_right &&
         left.reference_left == right.reference_left;
}

/// 只比较实际有效样本，缓冲剩余容量不属于输出音频。
bool EqualPlayback(const StereoPlaybackFrame& left, const StereoPlaybackFrame& right) {
  return left.frames == right.frames &&
         std::equal(left.pcm.begin(), left.pcm.begin() + left.frames * 2U, right.pcm.begin());
}

/// PCM 和采集元数据作为整体比较，防止音频正确而时间/参考错配一帧。
bool EqualClean(const CleanAudioFrame& left, const CleanAudioFrame& right) {
  return left.pcm == right.pcm && left.metadata.timestamp_us == right.metadata.timestamp_us &&
         left.metadata.input_dbfs == right.metadata.input_dbfs &&
         left.metadata.reference_active == right.metadata.reference_active;
}

/**
 * @brief 让三路可识别样本穿过真实 320→256→320 FIFO，验证固定一帧延迟与元数据对齐。
 * 两轮 Open/Close 检查历史清空，再注入帧内第二个 vendor 块失败，禁止输出半帧。
 */
bool DspFrameAlignment() {
  boompi::platform::rv1106::RockchipVoiceDsp dsp;
  CaptureChannels input{};
  CleanAudioFrame output{};
  for (unsigned session = 0U; session < 2U; ++session) {
    if (!Check(dsp.Open(), "real DSP FIFO adapter did not open")) {
      return false;
    }
    CleanAudioFrame expected{};
    for (unsigned period = 0U; period < 12U; ++period) {
      input.metadata = {1000000U + session * 1000000U + period * 20000U,
                        -40.0F + static_cast<float>(period), period % 2U != 0U};
      for (std::size_t i = 0U; i < kVoiceFrameSamples; ++i) {
        input.mic_left[i] = static_cast<std::int16_t>(period * 400U + i);
        input.mic_right[i] = static_cast<std::int16_t>(10U + period);
        input.reference_left[i] = -static_cast<std::int16_t>(period);
      }
      if (period == 0U) {
        expected.metadata.timestamp_us = input.metadata.timestamp_us;
      }
      if (!Check(dsp.Process(input, &output) && EqualClean(output, expected),
                 "PCM and raw-level/reference/time metadata did not share one-frame delay")) {
        return false;
      }
      expected.metadata = input.metadata;
      for (std::size_t i = 0U; i < kVoiceFrameSamples; ++i) {
        expected.pcm[i] = static_cast<std::int16_t>(input.mic_left[i] + 2 * input.mic_right[i] -
                                                    input.reference_left[i]);
      }
    }
    if (!Check(dsp_calls == 15U && !dsp.Process(input, nullptr) && dsp.IsOpen(),
               "256/320 block cadence or null-output lifecycle changed")) {
      return false;
    }
    dsp.Close();
    if (!Check(!dsp.Process(input, &output) && EqualClean(output, CleanAudioFrame{}),
               "closed DSP exposed stale PCM or metadata")) {
      return false;
    }
  }
  if (!dsp.Open()) {
    return false;
  }
  for (unsigned period = 0U; period < 3U; ++period) {
    if (!dsp.Process(input, &output)) {
      return false;
    }
  }
  // 第四个产品帧含两个vendor块；第二块失败时也不能交出半帧或旧元数据。
  dsp_failure_call = 5U;
  const bool failed = !dsp.Process(input, &output) && !dsp.IsOpen() && dsp_calls == 5U &&
                      EqualClean(output, CleanAudioFrame{});
  dsp_failure_call = 0U;
  CleanAudioFrame initial{};
  initial.metadata.timestamp_us = input.metadata.timestamp_us;
  return Check(failed, "vendor failure exposed a partial frame or kept the DSP active") &&
         Check(dsp.Open() && dsp.Process(input, &output) && EqualClean(output, initial),
               "reopen after failure retained FIFO or metadata history");
}

/**
 * @brief 真实四通道联合重采样：检查极性饱和、refR 隔离、直流剔除和重置可复现性。
 * 左麦满量程直流、右麦正弦和两个不同参考使串路/直流误判能被具体样本值发现。
 */
bool CaptureFormat() {
  AudioConverter converter, reference;
  if (!Check(converter.Open(-1, 1) && reference.Open(-1, 1) && converter.ResetCapture() &&
                 reference.ResetCapture(),
             "capture converter initialization failed")) {
    return false;
  }
  RawCaptureFrame raw{};
  raw.timestamp_us = 123456U;
  raw.discontinuity = true;
  for (std::size_t i = 0U; i < kCaptureFrameSamples; ++i) {
    raw.pcm[4U * i] = -32768;
    raw.pcm[4U * i + 1U] = static_cast<std::int16_t>(
        std::lround(8192.0 * std::sin(2.0 * 3.141592653589793 * i / 48.0)));
    raw.pcm[4U * i + 2U] = 80;
    raw.pcm[4U * i + 3U] = 30000;
  }
  const RawCaptureFrame original = raw;
  RawCaptureFrame other_reference = raw;
  for (std::size_t i = 0U; i < kCaptureFrameSamples; ++i) {
    other_reference.pcm[4U * i + 3U] = -20000;
  }
  CaptureChannels channels{}, other{}, first{};
  bool ok = true;
  for (unsigned period = 0U; period < 4U; ++period) {
    if (!Check(converter.ConvertCapture(raw, &channels) &&
                   reference.ConvertCapture(other_reference, &other),
               "joint resampling did not yield a complete 320-sample period")) {
      return false;
    }
    if (period == 0U) {
      first = channels;
    }
    if (period == 1U) {
      ok &= Check(converter.ResetPlayback(), "playback prime failed during capture");
    }
    ok &= Check(EqualChannels(channels, other), "refR leaked into microphone or refL planes");
  }
  ok &= Check(raw.pcm == original.pcm && raw.timestamp_us == original.timestamp_us &&
                  raw.discontinuity == original.discontinuity,
              "conversion modified the raw capture frame");
  ok &= Check(channels.mic_left.back() > 32760 && channels.reference_left.back() == 80,
              "microphone polarity saturated incorrectly or changed hardware reference");
  ok &= Check(std::abs(channels.metadata.input_dbfs + 15.0515F) < 0.3F,
              "raw-microphone AC RMS did not exclude DC or preserve right-channel level");
  ok &= Check(
      channels.metadata.timestamp_us == raw.timestamp_us && channels.metadata.reference_active,
      "capture metadata does not describe this input period");
  RawCaptureFrame silent_reference = raw;
  for (std::size_t i = 0U; i < kCaptureFrameSamples; ++i) {
    silent_reference.pcm[4U * i + 2U] = 0;
  }
  for (unsigned period = 0U; period < 4U; ++period) {
    ok &= converter.ConvertCapture(silent_reference, &channels);
  }
  ok &= Check(!channels.metadata.reference_active &&
                  std::all_of(channels.reference_left.begin(), channels.reference_left.end(),
                              [](std::int16_t value) {
                                return value == 0;
                              }),
              "refR alone was treated as the active AEC reference");
  ok &= Check(converter.ResetCapture() && converter.ConvertCapture(raw, &channels) &&
                  EqualChannels(channels, first),
              "capture reset retained old resampler history");
  converter.Close();
  ok &= Check(converter.Open(1, -1) && converter.ResetCapture(),
              "converter could not reopen with the other microphone polarity");
  for (std::size_t i = 0U; i < kCaptureFrameSamples; ++i) {
    raw.pcm[4U * i] = raw.pcm[4U * i + 1U];
  }
  for (unsigned period = 0U; period < 4U; ++period) {
    ok &= converter.ConvertCapture(raw, &channels);
  }
  for (std::size_t i = 0U; i < channels.mic_left.size(); ++i) {
    ok &= Check(std::abs(static_cast<int>(channels.mic_left[i]) + channels.mic_right[i]) <= 1,
                "microphone polarity changed alignment or failed to invert the right channel");
  }
  return ok;
}

/**
 * @brief 真实16→48k双声道转换，检查完整/短尾输入、滤波尾音、音量与重置。
 * 最后一采样点才出现脉冲，只有EOS正确排空滤波器时才会出现在播放流中。
 */
bool PlaybackFormat() {
  AudioConverter converter, reference;
  if (!Check(converter.Open(1, 1) && reference.Open(1, 1),
             "playback converter initialization failed")) {
    return false;
  }
  bool ok = true;
  for (const std::size_t samples : {std::size_t{1U}, std::size_t{73U}, kTtsFrameSamples}) {
    std::array<std::int16_t, kTtsFrameSamples> input{};
    input[samples - 1U] = 16000;
    StereoPlaybackFrame actual{}, expected{};
    ok &=
        Check(converter.ResetPlayback() && reference.ResetPlayback(), "playback reset failed");
    ok &= Check(converter.UpsamplePlayback(input.data(), samples, &actual) &&
                    reference.UpsamplePlayback(input.data(), samples, &expected) &&
                    EqualPlayback(actual, expected),
                "playback reset retained previous reply or did not produce identical stereo");
    std::vector<std::int16_t> rendered(actual.pcm.begin(),
                                       actual.pcm.begin() + actual.frames * 2U);
    ok &= Check(converter.ResetCapture(), "capture reset interfered with playback");
    // 排空必须在有限步结束，返回的frames均为每通道采样时刻。
    for (unsigned count = 0U; count < 4U; ++count) {
      ok &= Check(converter.UpsamplePlayback(nullptr, 0U, &actual), "EOS flush failed");
      rendered.insert(rendered.end(), actual.pcm.begin(),
                      actual.pcm.begin() + actual.frames * 2U);
      if (actual.frames == 0U) {
        break;
      }
    }
    ok &= Check(actual.frames == 0U && rendered.size() == samples * 3U * 2U,
                "EOS lost input duration or padded a short reply to a whole network frame");
    int peak = 0;
    for (std::size_t i = 0U; i < rendered.size(); i += 2U) {
      ok &= Check(rendered[i] == rendered[i + 1U], "left and right channels differ");
      peak = std::max(peak, std::abs(static_cast<int>(rendered[i])));
    }
    ok &= Check(peak > 8000, "final input impulse was lost in the resampler tail");
  }
  std::array<std::int16_t, kTtsFrameSamples> full{};
  full.fill(9000);
  StereoPlaybackFrame actual{};
  ok &= Check(converter.ResetPlayback(), "constant playback reset failed");
  for (unsigned count = 0U; count < 4U; ++count) {
    ok &= converter.UpsamplePlayback(full.data(), full.size(), &actual);
  }
  ok &= Check(actual.frames == kCaptureFrameSamples && actual.pcm[400U] == 9000,
              "stereo matrix changed amplitude or steady-state duration");
  // 未排空就取消：下一次reset后的静音必须不含旧轮次留在滤波器中的尾音。
  full.fill(0);
  ok &= Check(converter.ResetPlayback() &&
                  converter.UpsamplePlayback(full.data(), full.size(), &actual) &&
                  AudioConverter::Peak(actual) == 0 &&
                  converter.UpsamplePlayback(nullptr, 0U, &actual) &&
                  AudioConverter::Peak(actual) == 0,
              "cancel/reset allowed the old reply's filter tail into the next reply");
  ok &= Check(!converter.UpsamplePlayback(nullptr, 1U, &actual) &&
                  !converter.UpsamplePlayback(full.data(), 0U, &actual) &&
                  !converter.UpsamplePlayback(full.data(), full.size() + 1U, &actual),
              "invalid playback buffer was accepted");
  StereoPlaybackFrame volume{};
  volume.frames = 2U;
  volume.pcm[0] = -32768;
  volume.pcm[1] = 32767;
  volume.pcm[2] = -1000;
  volume.pcm[3] = 1000;
  const long peak = AudioConverter::Peak(volume);
  AudioConverter::ApplyVolume(&volume, 2.0F, peak);
  ok &= Check(peak == 32768 && volume.pcm[0] == -31128 && AudioConverter::Peak(volume) <= 31128,
              "gain limiter lost negative full scale or exceeded its 95 percent ceiling");
  AudioConverter::ApplyVolume(&volume, 0.0F, AudioConverter::Peak(volume));
  ok &= Check(AudioConverter::Peak(volume) == 0, "zero gain did not silence playback");
  volume.pcm[0] = -1000;
  volume.pcm[1] = 1000;
  AudioConverter::ApplyVolume(&volume, 0.5F, AudioConverter::Peak(volume));
  ok &= Check(volume.pcm[0] == -500 && volume.pcm[1] == 500,
              "ordinary volume gain changed outside the limiter range");
  AudioConverter::ApplyVolume(&volume, std::numeric_limits<float>::quiet_NaN(),
                              AudioConverter::Peak(volume));
  ok &= Check(AudioConverter::Peak(volume) == 0, "non-finite gain did not silence playback");
  return ok;
}

}  // namespace

// 仅实现第三方的窄C边界。分类结果可编排，生产的VAD滞回、dBFS和AEC门控不可替换。
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
  // 校验包装层传来的基本配置，返回参数地址作不透明句柄；不创建真实算法实例。
  dsp_calls = 0U;
  return rate == 16000 && bits == 16 && microphones == 2 && references == 1 ? parameters
                                                                            : nullptr;
}
int rkaudio_preprocess_short(void* handle, short* input, short* output, int count, int*) {
  ++dsp_calls;
  if (handle == nullptr || count != 768 || dsp_calls == dsp_failure_call) {
    return 0;
  }
  // 固定可预测的三路组合，用于发现FIFO乱序、重复或通道交换，不模拟AEC。
  for (int i = 0; i < 256; ++i) {
    output[i] = static_cast<short>(input[3 * i] + 2 * input[3 * i + 1] - input[3 * i + 2]);
  }
  // vendor 返回值按字节计：256 个 S16 输出样本对应 512 字节。
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
  return rate == 16000 && samples == 320U ? 0 : -1;
}
int WebRtcVad_Process(VadInst* vad, int rate, const std::int16_t* pcm, std::size_t samples) {
  return vad != nullptr && pcm != nullptr &&
                 WebRtcVad_ValidRateAndFrameLength(rate, samples) == 0
             ? vad_result
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
  if (handle == nullptr || pcm == nullptr || samples != 320U || result == nullptr ||
      !snowboy_process_ok) {
    return 0;
  }
  *result = wake_result;
  return 1;
}
}

namespace boompi::test {

/** @brief 汇总转换、音量和 DSP 帧适配验证，任一契约失败即停止本场景。 */
bool TestPipelineFormat() {
  return CaptureFormat() && PlaybackFormat() && DspFrameAlignment();
}

/**
 * @brief 用可控 Snowboy/VAD 结果驱动真实 SpeechDetector，逐帧验证时间边界。
 * 每个 step 都按产品顺序先 Detect 后 GateNearVoice，一次推进严格对应 20 ms。
 */
bool TestPipelineDetection() {
  SpeechDetector detector;
  if (!Check(detector.Open() && detector.ResetListener(), "detector could not open")) {
    return false;
  }
  CleanAudioFrame clean{};
  for (std::size_t i = 0U; i < clean.pcm.size(); ++i) {
    clean.pcm[i] = i % 2U == 0U ? 4096 : -4096;
  }
  clean.metadata = {100000U, -20.0F, true};
  CaptureFrame frame{};
  PlaybackState playback{};
  auto step = [&] {
    clean.metadata.timestamp_us += 20000U;
    return detector.Detect(clean, &frame) && detector.GateNearVoice(playback, &frame);
  };
  bool ok = true;
  wake_result = 2;
  // 先污染输出，核对 Detect 清旧事件且不再次延迟已经对齐的 PCM/元数据。
  frame.sequence = 99U;
  frame.discontinuity = frame.actor_overrun = frame.vad_ended = true;
  ok &= Check(step() && frame.pcm == clean.pcm && frame.wake &&
                  frame.timestamp_us == clean.metadata.timestamp_us &&
                  frame.input_dbfs == clean.metadata.input_dbfs && frame.reference_active &&
                  !frame.discontinuity && !frame.actor_overrun && !frame.vad_ended &&
                  frame.sequence == 0U && std::abs(frame.voice_dbfs + 18.0618F) < 0.01F,
              "clean PCM/metadata were delayed, altered, or contaminated by old flags");
  wake_result = 0;
  ok &= detector.ResetListener();
  // 开口门限只在入句前生效：-31 dBFS 不入句，-30 dBFS 连续六帧入句，轻尾字保留。
  clean.metadata.input_dbfs = -31.0F;
  for (unsigned i = 0U; i < 7U; ++i) {
    ok &= Check(step() && !frame.vad_now && !frame.vad_started,
                "sub-threshold raw microphone admitted a new utterance");
  }
  clean.metadata.input_dbfs = -30.0F;
  for (unsigned i = 0U; i < 6U; ++i) {
    ok &= Check(step() && frame.vad_now && frame.vad_started == (i == 5U),
                "VAD start did not require exactly 120ms of consecutive speech");
  }
  clean.metadata.input_dbfs = -80.0F;
  ok &= Check(step() && frame.vad_now, "quiet trailing speech was rejected after admission");
  vad_result = 0;
  for (unsigned i = 0U; i < 35U; ++i) {
    ok &= Check(step() && frame.vad_ended == (i == 34U), "VAD end is not the 700ms edge");
  }
  vad_result = 1;
  ok &= Check(step() && !frame.vad_now, "old utterance admission survived its end");

  ok &= detector.ResetListener();
  detector.ArmPlayback();
  clean.metadata.input_dbfs = -20.0F;
  // 渲染未开始/参考未到都不消耗预热；参考出现后精确抑制 30 帧，自然尾音再抑制 15 帧。
  clean.metadata.reference_active = false;
  for (unsigned i = 0U; i < 40U; ++i) {
    ok &= Check(step() && !frame.near_voice, "AEC warmup advanced before rendering began");
  }
  playback.render_started = playback.output_audible = true;
  for (unsigned i = 0U; i < 5U; ++i) {
    ok &= Check(step() && !frame.near_voice, "AEC warmup advanced without hardware reference");
  }
  clean.metadata.reference_active = true;
  for (unsigned i = 0U; i < 30U; ++i) {
    ok &= Check(step() && !frame.near_voice, "AEC warmup did not suppress all 30 frames");
  }
  ok &= Check(step() && frame.near_voice, "AEC warmup never released near speech");
  playback.end = PlaybackEnd::Natural;
  clean.metadata.reference_active = false;
  for (unsigned i = 0U; i < 15U; ++i) {
    ok &= Check(step() && !frame.near_voice, "natural playback tail is not 15 frames");
    playback.end = PlaybackEnd::None;
  }
  ok &= Check(step() && frame.near_voice, "natural playback tail never released speech");

  ok &= detector.ResetListener();
  detector.ArmPlayback();
  playback.output_audible = false;
  // 静音先旁路，恢复音量后必须重新等参考并预热，防止沿用静音时期的放行状态。
  ok &= Check(step() && frame.near_voice, "zero-volume playback permanently blocked speech");
  playback.output_audible = true;
  ok &= Check(step() && !frame.near_voice, "raising volume did not rearm AEC protection");
  clean.metadata.reference_active = true;
  for (unsigned i = 0U; i < 30U; ++i) {
    ok &= Check(step() && !frame.near_voice, "audible playback bypassed renewed warmup");
  }
  for (unsigned i = 0U; i < 6U; ++i) {
    ok &= Check(step() && frame.near_voice, "speech was unavailable after renewed warmup");
  }
  // 采集断点后由后端重置前端；这里验证播放保护必须重新等待/预热，而非沿用旧收敛状态。
  detector.OnDiscontinuity();
  ok &= detector.ResetListener();
  for (unsigned i = 0U; i < 30U; ++i) {
    ok &= Check(step() && !frame.near_voice, "capture discontinuity bypassed AEC warmup");
  }
  for (unsigned i = 0U; i < 6U; ++i) {
    ok &=
        Check(step() && frame.near_voice, "speech did not recover after capture discontinuity");
  }
  playback.end = PlaybackEnd::Interrupted;
  clean.metadata.input_dbfs = -80.0F;
  ok &= Check(step() && frame.near_voice && frame.vad_now,
              "active interruption reset accepted speech or added a tail window");
  playback.end = PlaybackEnd::None;
  vad_result = 0;
  for (unsigned i = 0U; i < 35U; ++i) {
    ok &= Check(step() && frame.vad_ended == (i == 34U),
                "interrupted utterance lost its VAD end lifecycle");
  }
  vad_result = -1;
  // 分类库失败不能被当成普通静音；最后分别检查 VAD 和 Snowboy 的错误传播。
  ok &= Check(!detector.Detect(clean, &frame), "VAD processing failure was hidden");
  vad_result = 1;
  snowboy_process_ok = false;
  ok &= Check(!detector.Detect(clean, &frame), "Snowboy processing failure was hidden");
  snowboy_process_ok = true;
  detector.Close();
  return ok;
}

}  // namespace boompi::test
