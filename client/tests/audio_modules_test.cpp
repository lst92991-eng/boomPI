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
struct CleanAudioFrame {
  boompi::audio::VoiceFrame16k pcm{};
  boompi::audio::CaptureMetadata metadata{};
};
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
bool EqualClean(const CleanAudioFrame& left, const CleanAudioFrame& right) {
  return left.pcm == right.pcm && left.metadata.timestamp_us == right.metadata.timestamp_us &&
         left.metadata.input_dbfs == right.metadata.input_dbfs &&
         left.metadata.reference_active == right.metadata.reference_active;
}

bool DspFrameAlignment() {
  CaptureChannels input{};
  CleanAudioFrame output{};
  for (unsigned session = 0; session < 2; ++session) {
    if (!Check(dsp::open(), "3A open failed")) {
      return false;
    }
    CleanAudioFrame expected{};
    for (unsigned period = 0; period < 12; ++period) {
      input.metadata = {1000000U + session * 1000000U + period * 20000U,
                        -40.0F + static_cast<float>(period), period % 2U != 0};
      for (std::size_t i = 0; i < kVoiceFrameSamples; ++i) {
        input.mic_left[i] = static_cast<std::int16_t>(period * 400 + i);
        input.mic_right[i] = static_cast<std::int16_t>(10 + period);
        input.reference_left[i] = -static_cast<std::int16_t>(period);
      }
      if (period == 0) {
        expected.metadata.timestamp_us = input.metadata.timestamp_us;
      }
      if (!Check(dsp::process(input, &output.pcm, &output.metadata) &&
                     EqualClean(output, expected),
                 "3A PCM and metadata lost their shared one-frame delay")) {
        dsp::close();
        return false;
      }
      expected.metadata = input.metadata;
      for (std::size_t i = 0; i < kVoiceFrameSamples; ++i) {
        expected.pcm[i] = static_cast<std::int16_t>(input.mic_left[i] + 2 * input.mic_right[i] -
                                                    input.reference_left[i]);
      }
    }
    const bool cadence = dsp_calls == 15 && !dsp::process(input, nullptr, &output.metadata);
    dsp::close();
    if (!Check(cadence && !dsp::process(input, &output.pcm, &output.metadata) &&
                   EqualClean(output, {}),
               "3A block cadence or close failed")) {
      return false;
    }
  }
  if (!dsp::open()) {
    return false;
  }
  for (unsigned i = 0; i < 3; ++i) {
    if (!dsp::process(input, &output.pcm, &output.metadata)) {
      dsp::close();
      return false;
    }
  }
  dsp_failure_call = 5;
  const bool failed = !dsp::process(input, &output.pcm, &output.metadata) && dsp_calls == 5 &&
                      EqualClean(output, {});
  dsp_failure_call = 0;
  CleanAudioFrame initial{};
  initial.metadata.timestamp_us = input.metadata.timestamp_us;
  const bool reopened = dsp::open() && dsp::process(input, &output.pcm, &output.metadata) &&
                        EqualClean(output, initial);
  dsp::close();
  return Check(failed && reopened, "3A partial failure leaked output or survived reopen");
}

bool CaptureFormat() {
  if (!convert::open_capture(-1, 1)) {
    return false;
  }
  RawCaptureFrame raw{};
  for (std::size_t i = 0; i < kCaptureFrameSamples; ++i) {
    raw.pcm[4 * i] = -32768;
    raw.pcm[4 * i + 1] = static_cast<std::int16_t>(8000 * std::sin(i * 0.13));
    raw.pcm[4 * i + 2] = static_cast<std::int16_t>(3000 * std::cos(i * 0.07));
    raw.pcm[4 * i + 3] = 27000;
  }
  raw.timestamp_us = 123456;
  const auto saved_raw = raw.pcm;
  CaptureChannels first{}, repeated{}, no_ref{};
  bool ok = convert::reset_capture() && convert::capture(raw, &first) &&
            convert::reset_capture() && convert::capture(raw, &repeated);
  ok &= Check(first.mic_left == repeated.mic_left && first.mic_right == repeated.mic_right &&
                  first.reference_left == repeated.reference_left && raw.pcm == saved_raw,
              "capture reset changed channel alignment or modified raw samples");
  ok &= Check(first.mic_left.back() > 32000 && first.metadata.reference_active &&
                  first.metadata.timestamp_us == raw.timestamp_us &&
                  first.metadata.input_dbfs > -20,
              "capture polarity, metadata or reference was lost");
  for (std::size_t i = 0; i < kCaptureFrameSamples; ++i) {
    raw.pcm[4 * i + 2] = 0;
  }
  ok &= convert::reset_capture() && convert::capture(raw, &no_ref);
  ok &= Check(!no_ref.metadata.reference_active &&
                  std::all_of(no_ref.reference_left.begin(), no_ref.reference_left.end(),
                              [](auto value) {
                                return value == 0;
                              }),
              "refR leaked into the single AEC reference");
  VoiceFrame16k dc{};
  for (std::size_t i = 0; i < kCaptureFrameSamples; ++i) {
    const auto value = static_cast<std::int16_t>(4000 * std::sin(i * 0.13));
    raw.pcm[4 * i] = -value;
    raw.pcm[4 * i + 1] = value;
    raw.pcm[4 * i + 2] = value;
    raw.pcm[4 * i + 3] = 30000;
  }
  ok &= Check(convert::reset_capture() && convert::capture(raw, &first) &&
                  first.mic_left == first.mic_right && first.mic_left == first.reference_left,
              "three used channels lost common filter phase or refR was mixed in");
  dc.fill(10000);
  ok &= Check(convert::ac_rms_dbfs(dc) == -120, "DC offset admitted speech");
  convert::close_capture();
  return ok;
}

bool PlaybackFormat() {
  if (!convert::open_playback()) {
    return false;
  }
  bool ok = true;
  for (const std::size_t count : {std::size_t{1}, std::size_t{73}, kTtsFrameSamples}) {
    std::array<std::int16_t, kTtsFrameSamples> input{};
    input[count - 1] = 16000;
    StereoPlaybackFrame output{};
    ok &= convert::reset_playback() && convert::playback(input.data(), count, &output);
    std::vector<std::int16_t> rendered(output.pcm.begin(),
                                       output.pcm.begin() + output.frames * 2);
    for (unsigned flush = 0; flush < 4; ++flush) {
      ok &= Check(convert::playback(nullptr, 0, &output), "EOS flush failed");
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
  std::array<std::int16_t, kTtsFrameSamples> full{};
  full.fill(9000);
  StereoPlaybackFrame output{};
  ok &= convert::reset_playback();
  for (unsigned i = 0; i < 4; ++i) {
    ok &= convert::playback(full.data(), full.size(), &output);
  }
  ok &= Check(output.frames == kCaptureFrameSamples && output.pcm[400] == 9000,
              "steady-state resampling changed duration or volume");
  full.fill(0);
  ok &=
      Check(convert::reset_playback() && convert::playback(full.data(), full.size(), &output) &&
                convert::peak(output) == 0 && convert::playback(nullptr, 0, &output) &&
                convert::peak(output) == 0,
            "cancel reset leaked old filter history");
  ok &= Check(!convert::playback(nullptr, 1, &output) &&
                  !convert::playback(full.data(), 0, &output) &&
                  !convert::playback(full.data(), full.size() + 1, &output),
              "invalid resampler input was accepted");
  output.frames = 2;
  output.pcm[0] = -32768;
  output.pcm[1] = 32767;
  output.pcm[2] = -1000;
  output.pcm[3] = 1000;
  convert::apply_volume(&output, 2, convert::peak(output));
  ok &= Check(output.pcm[0] == -31128 && convert::peak(output) <= 31128,
              "peak limiting lost negative full scale");
  convert::apply_volume(&output, std::numeric_limits<float>::quiet_NaN(),
                        convert::peak(output));
  ok &= Check(convert::peak(output) == 0, "invalid gain was not muted");
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
  speech::reset();
  if (!wake::open() || !vad::open()) {
    wake::close();
    vad::close();
    return false;
  }
  CaptureFrame frame{};
  for (std::size_t i = 0; i < frame.pcm.size(); ++i) {
    frame.pcm[i] = i % 2 == 0 ? 4096 : -4096;
  }
  frame.input_dbfs = -20;
  frame.reference_active = true;
  playback::Observation observation{};
  auto step = [&] {
    if (!wake::detect(frame.pcm, &frame.wake)) {
      return false;
    }
    const int result = vad::process(frame.pcm);
    if (result < 0) {
      return false;
    }
    frame.vad_now = result == 1;
    return speech::update(frame, false, observation).error == nullptr;
  };
  bool ok = true;
  wake_result = 2;
  ok &= Check(step() && frame.wake, "Snowboy wake result was lost");
  wake_result = 0;
  ok &= wake::reset() && vad::reset();
  frame.input_dbfs = -31;
  for (unsigned i = 0; i < 7; ++i) {
    ok &= Check(step() && !frame.vad_now && !frame.vad_started,
                "below-threshold microphone admitted speech");
  }
  frame.input_dbfs = -30;
  for (unsigned i = 0; i < 6; ++i) {
    ok &= Check(step() && frame.vad_now && frame.vad_started == (i == 5),
                "speech start is not exactly 120ms");
  }
  frame.input_dbfs = -80;
  ok &= Check(step() && frame.vad_now, "quiet trailing speech disappeared");
  vad_result = 0;
  for (unsigned i = 0; i < 35; ++i) {
    ok &= Check(step() && frame.vad_ended == (i == 34), "VAD end is not 700ms");
  }
  vad_result = 1;
  ok &= Check(step() && !frame.vad_now, "ended admission survived");
  ok &= vad::reset();
  speech::reply_started();
  frame.input_dbfs = -20;
  frame.reference_active = false;
  for (unsigned i = 0; i < 40; ++i) {
    ok &= Check(step() && !frame.near_voice, "warmup advanced before rendering");
  }
  observation.render_started = observation.output_audible = true;
  for (unsigned i = 0; i < 5; ++i) {
    ok &= Check(step() && !frame.near_voice, "warmup advanced without reference");
  }
  frame.reference_active = true;
  for (unsigned i = 0; i < 30; ++i) {
    ok &= Check(step() && !frame.near_voice, "AEC warmup did not last 30 frames");
  }
  ok &= Check(step() && frame.near_voice, "warmup never released speech");
  observation.end = playback::End::Natural;
  frame.reference_active = false;
  for (unsigned i = 0; i < 15; ++i) {
    ok &= Check(step() && !frame.near_voice, "natural tail did not last 15 frames");
    observation.end = playback::End::None;
  }
  ok &= Check(step() && frame.near_voice, "natural tail never released speech");
  ok &= vad::reset();
  speech::reply_started();
  observation.output_audible = false;
  ok &= Check(step() && frame.near_voice, "zero-volume playback blocked speech");
  observation.output_audible = true;
  ok &= Check(step() && !frame.near_voice, "volume restore did not rearm AEC");
  frame.reference_active = true;
  for (unsigned i = 0; i < 30; ++i) {
    ok &= Check(step() && !frame.near_voice, "audible playback bypassed warmup");
  }
  for (unsigned i = 0; i < 6; ++i) {
    ok &= Check(step() && frame.near_voice, "speech unavailable after warmup");
  }
  frame.discontinuity = true;
  speech::update(frame, true, observation);
  frame.discontinuity = false;
  ok &= vad::reset();
  for (unsigned i = 0; i < 30; ++i) {
    ok &= Check(step() && !frame.near_voice, "capture break bypassed renewed warmup");
  }
  for (unsigned i = 0; i < 6; ++i) {
    ok &= Check(step() && frame.near_voice, "speech unavailable after capture break");
  }
  observation.end = playback::End::Interrupted;
  frame.input_dbfs = -80;
  ok &= Check(step() && frame.near_voice && frame.vad_now,
              "interrupt reset accepted speech or added a tail window");
  observation.end = playback::End::None;
  vad_result = 0;
  for (unsigned i = 0; i < 35; ++i) {
    ok &= Check(step() && frame.vad_ended == (i == 34), "interrupt lost utterance END");
  }
  vad_result = -1;
  ok &= Check(!step(), "VAD failure became silence");
  vad_result = 1;
  snowboy_process_ok = false;
  ok &= Check(!step(), "Snowboy failure was hidden");
  snowboy_process_ok = true;
  vad::close();
  wake::close();
  return ok;
}
}  // namespace boompi::test
