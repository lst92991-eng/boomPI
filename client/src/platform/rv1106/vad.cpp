#include "vad.h"

#include <algorithm>
#include <cstdio>
extern "C" {
#include <webrtc_vad.h>
}
#include "audio_convert.h"
#include "board_voice_profile.h"

namespace boompi::vad {
namespace {
VadInst* detector{nullptr};
std::uint32_t speech_ms{0U}, silence_ms{0U};
unsigned suppress_remaining{0U}, reference_wait_frames{0U};
bool in_speech{false}, playback_active{false}, warmup_armed{false}, silent_bypass{false};
const char* failure{""};
bool Fail(const char* why) noexcept {
  failure = why;
  return false;
}
constexpr std::uint32_t kFrameMs = audio::VoiceFrameContract::frame_ms;
// 连续 6 帧人声确认开口，连续 35 帧非人声确认收尾，容纳句内的短暂停顿。
constexpr std::uint32_t kVadStartMs = 120U, kVadEndMs = 700U;
// WebRTC VAD 模式 3 使用最积极的非语音过滤；原始麦电平准入仍独立判断。
constexpr int kVadMode = 3;
// 首次真实参考到达后抑制 600 ms；自然播完后抑制 300 ms，分别覆盖预热和尾音。
constexpr unsigned kAecWarmupFrames =
    static_cast<unsigned>(audio::VoiceFrameContract::FramesForMs(600U));
constexpr unsigned kAecTailFrames =
    static_cast<unsigned>(audio::VoiceFrameContract::FramesForMs(300U));
}  // namespace

bool open() noexcept {
  if (detector != nullptr) {
    return Fail("WebRTC VAD is already open");
  }
  detector = WebRtcVad_Create();
  if (detector == nullptr || WebRtcVad_Init(detector) != 0 ||
      WebRtcVad_set_mode(detector, kVadMode) != 0 ||
      WebRtcVad_ValidRateAndFrameLength(16000, audio::kVoiceFrameSamples) != 0) {
    close();
    return Fail("WebRTC VAD initialization failed");
  }
  failure = "";
  return true;
}

bool reset() noexcept {
  // 抑制窗结束时一起清库内历史和外部计数，不能让被屏蔽的回声续成一句话。
  if (detector == nullptr || WebRtcVad_Init(detector) != 0 ||
      WebRtcVad_set_mode(detector, kVadMode) != 0) {
    return false;
  }
  speech_ms = silence_ms = 0U;
  in_speech = false;
  return true;
}

void arm_playback() noexcept {
  // 新一轮不继承旧轮次的尾音窗口；先等待渲染与参考，首播蓄水不消耗预热帧数。
  playback_active = true;
  warmup_armed = true;
  silent_bypass = false;
  suppress_remaining = 0U;
  reference_wait_frames = 0U;
}

void discontinuity() noexcept {
  // 采集断点使 3A 的收敛历史失效；这里只重新启用活动轮次的参考等待。
  // 重采样、3A 和检测历史的实际复位由采集任务顺序完成。
  if (playback_active) {
    warmup_armed = true;
    suppress_remaining = 0U;
  }
}

static bool GateNearVoice(const playback::Observation& observation,
                          audio::CaptureFrame* frame) noexcept {
  if (frame == nullptr || detector == nullptr) {
    return Fail("speech detector is not ready");
  }
  bool suppress = false;
  // 优先处理一次性的结束事件，避免同帧的旧渲染标志重新开启预热。
  if (observation.end != playback::End::None) {
    playback_active = false;
    warmup_armed = false;
    silent_bypass = false;
    // 主动打断保留已确认的近讲VAD；自然播完才隔离扬声器与房间尾音。
    if (observation.end == playback::End::Interrupted) {
      suppress_remaining = 0U;
    } else {
      suppress_remaining = kAecTailFrames;
      suppress = true;
    }
  } else if (silent_bypass && observation.output_audible) {
    // 静音期间恢复音量后，重新等待真实参考，不能沿用无回声源时的旁路。
    silent_bypass = false;
    warmup_armed = true;
    reference_wait_frames = 0U;
    suppress = true;
  } else if (warmup_armed) {
    suppress = true;
    if (!observation.render_started) {
      reference_wait_frames = 0U;
    } else if (!observation.output_audible) {
      // 将零增益渲染视为静音，暂时跳过参考等待，允许输出近讲候选。
      warmup_armed = false;
      silent_bypass = true;
      reference_wait_frames = 0U;
      suppress = false;
    } else if (frame->reference_active) {
      // 渲染先于 ALSA 写入发布，必须等采集参考出现，才能开始计算收敛保护时间。
      warmup_armed = false;
      suppress_remaining = kAecWarmupFrames;
      reference_wait_frames = 0U;
    } else if (++reference_wait_frames == 50U) {
      // 50 个 20 ms 帧仅触发告警；缺参考时继续抑制，不因等待超时而放行回声。
      std::fprintf(stderr,
                   "boompi-client: warning: AEC reference absent for 1000 ms of playback\n");
    }
  }
  // 窗口包含触发当帧，倒数最后一帧也不放行；清历史后从下一帧重新积累 VAD。
  if (suppress_remaining != 0U) {
    suppress = true;
    if (--suppress_remaining == 0U && !reset()) {
      return Fail("playback VAD reset failed");
    }
  }
  // 仅输出近讲候选，不改写本帧唤醒与语句边沿，也不在这里确认或执行打断。
  frame->near_voice = !suppress && frame->vad_now;
  return true;
}

bool process(audio::CaptureFrame* frame, const playback::Observation& observation) noexcept {
  if (frame == nullptr || detector == nullptr) {
    return Fail("WebRTC VAD is not ready");
  }
  frame->vad_started = frame->vad_ended = frame->near_voice = false;
  // 去除直流偏置后的 3A 输出电平供上层确认近讲，不替代原始麦的开口门限。
  frame->voice_dbfs = audio_convert::ac_rms_dbfs(frame->pcm);

  const int speech = WebRtcVad_Process(detector, 16000, frame->pcm.data(), frame->pcm.size());
  if (speech < 0) {
    return Fail("WebRTC VAD processing failed");
  }
  // 原始麦电平只控制开口准入；一句话开始后仍保留较轻的尾字。
  const bool admitted = in_speech || frame->input_dbfs >= audio::board::kSpeechAdmissionDbfs;
  frame->vad_now = speech == 1 && admitted;
  // 两个计数只累计连续帧，状态反转即清零另一侧，饱和后不再增长。
  if (frame->vad_now) {
    speech_ms = std::min(speech_ms + kFrameMs, kVadStartMs);
    silence_ms = 0U;
  } else {
    silence_ms = std::min(silence_ms + kFrameMs, kVadEndMs);
    speech_ms = 0U;
  }
  // 起止标志只在跨过门限的当帧置位；vad_now 仍保留未经起止去抖的逐帧判定。
  if (!in_speech && speech_ms >= kVadStartMs) {
    in_speech = true;
    frame->vad_started = true;
  } else if (in_speech && silence_ms >= kVadEndMs) {
    in_speech = false;
    frame->vad_ended = true;
  }
  return GateNearVoice(observation, frame);
}

void close() noexcept {
  if (detector != nullptr) {
    WebRtcVad_Free(detector);
  }
  detector = nullptr;
  speech_ms = silence_ms = 0U;
  suppress_remaining = 0U;
  reference_wait_frames = 0U;
  in_speech = false;
  playback_active = warmup_armed = silent_bypass = false;
  failure = "";
}

const char* error() noexcept {
  return failure;
}
}  // namespace boompi::vad
