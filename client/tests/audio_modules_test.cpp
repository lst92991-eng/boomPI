// 真实格式转换与厂商薄封装；仅vendor C核返回值可控，不修改生产模块私有状态。
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <vector>

#include "audio_convert.h"
#include "audio_vendor.h"
#include "boompi/audio/playback.h"
#include "boompi/audio/speech.h"
#include "boompi/platform/rv1106/rockchip_3a.h"
#include "vad.h"
#include "wake.h"

namespace {
using namespace boompi::audio;
using namespace boompi::test::audio_vendor;
namespace convert = boompi::audio_convert;
namespace dsp = boompi::rockchip_3a;
namespace vad = boompi::vad;
namespace speech = boompi::speech;
namespace wake = boompi::wake;
namespace playback = boompi::playback;

bool Check(bool condition, const char* text) {
  if (!condition) {
    std::fprintf(stderr, "audio formats: %s\n", text);
  }
  return condition;
}
bool DspFrameAlignment() {
  CaptureChannels input{};
  VoiceFrame16k output{};
  for (unsigned session = 0; session < 2; ++session) {
    if (!Check(dsp::open(), "3A open failed")) {
      return false;
    }
    VoiceFrame16k expected{};
    for (unsigned period = 0; period < 12; ++period) {
      for (std::size_t i = 0; i < kVoiceFrameSamples; ++i) {
        input[3 * i] = static_cast<std::int16_t>(period * 400 + i);
        input[3 * i + 1] = static_cast<std::int16_t>(10 + period);
        input[3 * i + 2] = -static_cast<std::int16_t>(period);
      }
      if (!Check(dsp::process(input, output) && output == expected,
                 "3A changed samples across 256/320 boundaries")) {
        dsp::close();
        return false;
      }

      for (std::size_t i = 0; i < kVoiceFrameSamples; ++i) {
        expected[i] =
            static_cast<std::int16_t>(input[3 * i] + 2 * input[3 * i + 1] - input[3 * i + 2]);
      }
    }
    const bool cadence = dsp_calls == 15;
    dsp::close();
    if (!Check(cadence, "3A block cadence or close failed")) {
      return false;
    }
  }
  if (!dsp::open()) {
    return false;
  }
  for (unsigned i = 0; i < 3; ++i) {
    if (!dsp::process(input, output)) {
      dsp::close();
      return false;
    }
  }
  dsp_failure_call = 5;
  const bool failed = !dsp::process(input, output) && dsp_calls == 5;
  dsp_failure_call = 0;
  VoiceFrame16k initial{};
  const bool reopened = dsp::open() && dsp::process(input, output) && output == initial;
  dsp::close();
  return Check(failed && reopened, "3A failure was hidden or leaked old history after reopen");
}

bool CaptureFormat() {
  if (!convert::open_capture()) {
    return false;
  }
  RawCaptureFrame raw{};
  for (std::size_t i = 0; i < kDeviceFrameSamples; ++i) {
    const auto value = static_cast<std::int16_t>(4000 * std::sin(i * 0.13));
    raw[4 * i] = raw[4 * i + 1] = raw[4 * i + 2] = value;
    raw[4 * i + 3] = 30000;
  }
  const auto saved = raw;
  CaptureChannels first{}, repeated{};
  bool ok = convert::capture(raw, first) && convert::reset_capture() &&
            convert::capture(raw, repeated);
  ok &= Check(first == repeated && raw == saved, "reset or conversion modified input");
  for (std::size_t i = 0; i < kVoiceFrameSamples; ++i) {
    ok &= Check(first[3 * i] == first[3 * i + 1] && first[3 * i] == first[3 * i + 2],
                "mic/reference phase mismatch or unused refR mixed in");
  }
  for (std::size_t i = 0; i < kDeviceFrameSamples; ++i) {
    raw[4 * i + 1] = raw[4 * i] * 2;
    raw[4 * i + 2] = raw[4 * i] * 3;
  }
  ok &= convert::reset_capture() && convert::capture(raw, first);
  for (std::size_t i = 0; i < kVoiceFrameSamples; ++i) {
    ok &= Check(std::abs(first[3 * i + 1] - 2 * first[3 * i]) <= 2 &&
                    std::abs(first[3 * i + 2] - 3 * first[3 * i]) <= 3,
                "capture channel matrix permuted or attenuated microphone/reference slots");
  }
  convert::close_capture();
  return ok;
}

bool PlaybackFormat() {
  if (!convert::open_playback()) {
    return false;
  }
  bool ok = true;
  for (const std::size_t count : {std::size_t{1}, std::size_t{73}, kVoiceFrameSamples}) {
    std::array<std::int16_t, kVoiceFrameSamples> input{};
    input[count - 1] = 16000;
    StereoPlaybackFrame output{};
    ok &= convert::reset_playback() && convert::playback(input.data(), count, output);
    std::vector<std::int16_t> rendered(output.pcm.begin(),
                                       output.pcm.begin() + output.frames * 2);
    for (unsigned flush = 0; flush < 4; ++flush) {
      ok &= Check(convert::playback(nullptr, 0, output), "EOS flush failed");
      rendered.insert(rendered.end(), output.pcm.begin(),
                      output.pcm.begin() + output.frames * 2);
      if (output.frames == 0) {
        break;
      }
    }
    ok &= Check(output.frames == 0 && rendered.size() == count * 3 * 2,
                "EOS lost samples or extended the source duration");
    int peak = 0;
    for (std::size_t i = 0; i < rendered.size(); i += 2) {
      ok &= Check(rendered[i] == rendered[i + 1], "stereo channels differ");
      peak = std::max(peak, std::abs(static_cast<int>(rendered[i])));
    }
    ok &= Check(peak > 8000, "last input impulse disappeared");
  }
  std::array<std::int16_t, kVoiceFrameSamples> full{};
  full.fill(9000);
  StereoPlaybackFrame output{};
  ok &= convert::reset_playback();
  for (unsigned i = 0; i < 4; ++i) {
    ok &= convert::playback(full.data(), full.size(), output);
  }
  ok &= Check(output.frames == kDeviceFrameSamples && output.pcm[400] == 9000,
              "steady-state resampling changed duration or volume");
  full.fill(0);
  ok &=
      Check(convert::reset_playback() && convert::playback(full.data(), full.size(), output) &&
                std::all_of(output.pcm.begin(), output.pcm.begin() + output.frames * 2,
                            [](auto x) {
                              return x == 0;
                            }) &&
                convert::playback(nullptr, 0, output) &&
                std::all_of(output.pcm.begin(), output.pcm.begin() + output.frames * 2,
                            [](auto x) {
                              return x == 0;
                            }),
            "cancel reset leaked old filter history");
  convert::close_playback();
  return ok;
}
}  // namespace

namespace boompi::test {
bool TestModuleFormat() {
  audio_vendor::reset();
  return CaptureFormat() && PlaybackFormat() && DspFrameAlignment();
}

bool TestModuleDetection() {
  audio_vendor::reset();
  if (!wake::open() || !vad::open()) {
    wake::close();
    vad::close();
    return false;
  }
  VoiceFrame16k pcm{};
  wake_result = 2;
  bool ok = Check(wake::detect(pcm) == 1, "Snowboy wake was lost");
  wake_result = -2;
  ok &= Check(wake::detect(pcm) == 0, "Snowboy silence was rejected");
  wake_result = -1;
  ok &= Check(wake::detect(pcm) == -1, "Snowboy error became silence");
  for (int result : {0, 1, -1}) {
    vad_result = result;
    ok &= Check(vad::process(pcm) == result, "VAD conflated error and silence");
  }
  snowboy_process_ok = false;
  ok &= Check(wake::detect(pcm) == -1, "Snowboy failure was hidden");
  ok &= wake::reset() && vad::reset();
  vad::close();
  wake::close();
  return ok;
}
}  // namespace boompi::test
