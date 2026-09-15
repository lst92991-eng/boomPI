/** @file audio_pipeline.cpp
 * @brief 顺序连接声卡、格式转换、3A和检测；播放走独立的数据链。
 *
 * 采集线程独占输入链，播放线程独占输出链。只有播放事实跨线程发布，
 * 检测历史仍由采集线程修改；本模块不创建线程，也不直接发送网络数据。
 * 可沿 ProcessCapture20ms 阅读 raw → channels → clean → frame 四种数据形态，
 * 沿 Render20ms 阅读 16 kHz PCM → 48k stereo → ALSA。原子播放事实由 Detect
 * 之后的 ReadPlaybackState 读取，再交给 GateNearVoice 处理当前近讲候选。
 */
#include "audio_pipeline.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <mutex>

#include "alsa_audio.h"
#include "audio_converter.h"
#include "board_voice_profile.h"
#include "boompi/platform/rv1106/rockchip_voice_dsp.h"
#include "speech_detector.h"

namespace boompi::platform::rv1106 {
namespace {
/// 对本地采集完成时刻取微秒观测值；只用于帧时间线，不解释为外部可比较的 UTC。
std::uint64_t MonotonicUs() noexcept {
  return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                        std::chrono::steady_clock::now().time_since_epoch())
                                        .count());
}
}  // namespace

/** @brief 成组拥有板端资源和预分配中间帧，Open/Close 与引擎线程生命周期对应。 */
bool AudioPipeline::Fail(const char* text) noexcept {
  std::lock_guard<std::mutex> lock(error_mutex);
  std::snprintf(error.data(), error.size(), "%s", text);
  return false;
}

void AudioPipeline::ClearError() noexcept {
  std::lock_guard<std::mutex> lock(error_mutex);
  error.fill('\0');
}

bool AudioPipeline::ResetFrontEnd() noexcept {
  if (!converter.ResetCapture()) {
    return Fail("capture resampler reset failed");
  }
  dsp.Close();
  if (!dsp.Open(audio::board::kAecDelaySamples)) {
    return Fail("Rockchip 3A initialization failed");
  }
  if (!detector.ResetListener()) {
    return Fail(detector.LastError());
  }
  return true;
}

PlaybackState AudioPipeline::ReadPlaybackState() noexcept {
  PlaybackState state;
  if (playback_ended.exchange(false)) {
    state.end = pcm.WasPlaybackInterrupted() ? PlaybackEnd::Interrupted : PlaybackEnd::Natural;
  }
  state.render_started = playback_render_started.load();
  state.output_audible = playback_output_audible.load();
  return state;
}

void AudioPipeline::PublishPlaybackEnd() noexcept {
  playback_ended.store(true);
  playback_render_started.store(false);
  playback_output_audible.store(false);
}
AudioPipeline::~AudioPipeline() noexcept {
  Close();
}

bool AudioPipeline::Open() noexcept {
  if (open) {
    return Fail("audio backend is already open");
  }
  ClearError();
  // 先建立声卡和格式转换器，再加载检测库，最后复位包含 3A 的前端；逐阶段回滚资源。
  if (!pcm.Open(audio::kCapturePcm, audio::kPlaybackPcm)) {
    return false;
  }
  if (!converter.Open(audio::board::kLeftMicPolarity, audio::board::kRightMicPolarity)) {
    Close();
    return Fail("audio converter initialization failed");
  }
  if (!detector.Open()) {
    const char* reason = detector.LastError();
    Close();
    return Fail(reason);
  }
  if (!ResetFrontEnd()) {
    Close();
    return false;
  }
  open = true;
  ClearError();
  return true;
}

bool AudioPipeline::ReadCapture20ms(RawCaptureFrame* const raw) noexcept {
  if (!open || raw == nullptr) {
    return false;
  }
  raw->discontinuity = false;
  if (!pcm.ReadCapture20ms(raw->pcm.data(), &raw->discontinuity)) {
    return false;
  }
  raw->timestamp_us = MonotonicUs();
  return true;
}

bool AudioPipeline::ProcessCapture20ms(const RawCaptureFrame& raw,
                                       CaptureFrame* const frame) noexcept {
  if (!open || frame == nullptr) {
    return false;
  }
  *frame = {};
  if (raw.discontinuity) {
    // 断点会使滤波和检测历史失效；该帧只传递取消意图，不继续送入后续处理。
    frame->discontinuity = true;
    detector.OnDiscontinuity();
    if (!ResetFrontEnd()) {
      return false;
    }
    return true;
  }

  // 1. 四通道一起降到 16 kHz，再取出双麦和 refL。
  if (!converter.ConvertCapture(raw, &channels)) {
    return Fail("capture resampler lost frame alignment");
  }
  // 2. 送入 Rockchip 3A，得到单声道；时间戳和参考标志一起对齐。
  if (!dsp.Process(channels, &clean)) {
    return Fail("Rockchip 3A rejected a frame");
  }
  // 3. 用同一帧清洁语音做唤醒检测和 VAD。
  if (!detector.Detect(clean, frame)) {
    return Fail(detector.LastError());
  }
  const PlaybackState playback = ReadPlaybackState();
  // 4. 根据播放参考屏蔽预热和尾音；是否确认插话由 VoiceAudio 决定。
  if (!detector.GateNearVoice(playback, frame)) {
    return Fail(detector.LastError());
  }
  return true;
}

bool AudioPipeline::ResetListener() noexcept {
  if (!open || !detector.ResetListener()) {
    return Fail("listener reset failed");
  }
  return true;
}

bool AudioPipeline::ArmPlayback() noexcept {
  if (!open) {
    return false;
  }
  // 仍由采集线程在帧边界执行，准备完成后才允许播放线程开始。
  detector.ArmPlayback();
  playback_ended.store(false);
  playback_render_started.store(false);
  playback_output_audible.store(false);
  pcm.ClearError();
  ClearError();
  return true;
}

bool AudioPipeline::PreparePlayback() noexcept {
  if (!open || !pcm.PreparePlayback()) {
    return false;
  }
  if (!converter.ResetPlayback()) {
    return Fail("playback resampler reset failed");
  }
  return true;
}

bool AudioPipeline::Render20ms(const std::int16_t* const input, const std::size_t samples,
                               const float gain) noexcept {
  if (!open || input == nullptr || samples == 0U || samples > audio::kTtsFrameSamples ||
      !std::isfinite(gain) || gain < 0.0F) {
    return false;
  }
  // 1. 直接把服务器的16k单声道转成声卡48k双声道，没有mono中转缓冲。
  if (!converter.UpsamplePlayback(input, samples, &stereo)) {
    return Fail("TTS resampling failed");
  }
  const long peak = AudioConverter::Peak(stereo);
  if (peak != 0) {
    // 标志在 ALSA write 前发布；零 PCM 不覆盖上一非零帧的状态，真实出声仍以 refL 为准。
    playback_render_started.store(true);
    playback_output_audible.store(gain > 0.0F);
  }
  // 2. 应用用户音量和插话探测的临时静音，并限制峰值。
  playback_gain = gain;
  AudioConverter::ApplyVolume(&stereo, gain, peak);
  // 3. 交给声卡；极短首包可能仍留在滤波器内，由DrainPlayback取出。
  return pcm.WritePlayback(stereo.pcm.data(), stereo.frames);
}

bool AudioPipeline::DrainPlayback() noexcept {
  if (!open) {
    return false;
  }
  // EOS先交付滤波器中的尾音，再等待ALSA。不能将网络结束当成声音已经播完。
  do {
    if (!converter.UpsamplePlayback(nullptr, 0U, &stereo)) {
      return Fail("TTS resampler tail failed");
    }
    const long peak = AudioConverter::Peak(stereo);
    if (peak != 0) {
      playback_render_started.store(true);
      playback_output_audible.store(playback_gain > 0.0F);
    }
    AudioConverter::ApplyVolume(&stereo, playback_gain, peak);
    if (!pcm.WritePlayback(stereo.pcm.data(), stereo.frames)) {
      return false;
    }
  } while (stereo.frames != 0U);
  const bool drained = pcm.DrainPlayback();
  PublishPlaybackEnd();
  return drained;
}
void AudioPipeline::DropPlayback() noexcept {
  pcm.DropPlayback();
  PublishPlaybackEnd();
}
void AudioPipeline::InterruptCapture() noexcept {
  pcm.InterruptCapture();
}
void AudioPipeline::InterruptPlayback() noexcept {
  pcm.InterruptPlayback();
  PublishPlaybackEnd();
}
std::string AudioPipeline::LastError() const {
  std::lock_guard<std::mutex> lock(error_mutex);
  return error[0] != '\0' ? std::string(error.data()) : pcm.LastError();
}
void AudioPipeline::Close() noexcept {
  // 调用方已经 join：此时才能释放同时被两条链引用的设备、转换器及各自缓冲。
  pcm.Close();
  converter.Close();
  dsp.Close();
  detector.Close();
  open = false;
  playback_render_started.store(false);
  playback_output_audible.store(false);
  playback_ended.store(false);
  channels = {};
  clean = {};
  stereo = {};
}

}  // namespace boompi::platform::rv1106
