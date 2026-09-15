/**
 * @file speech_detector.cpp
 * @brief 唤醒和语句边沿检测，以及播放参考驱动的近讲门控。
 *
 * Snowboy 与 WebRTC VAD 使用同一份 3A 输出；原始麦电平用于开口准入，
 * 3A 后电平随帧交给 VoiceAudio。播放保护按采集帧推进，不读取墙上时钟。
 * Detect 的 wake/vad_started/vad_ended 是事实输出；GateNearVoice 只为当前 near_voice
 * 增加保护。最终句首回补、400 ms 追问准入和静音探测确认在 VoiceAudio 中继续完成。
 */
#include "speech_detector.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "audio_converter.h"
#include "board_voice_profile.h"

namespace boompi::platform::rv1106 {
namespace {
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

SpeechDetector::~SpeechDetector() noexcept {
  Close();
}

bool SpeechDetector::Fail(const char* reason) noexcept {
  error_ = reason;
  return false;
}

bool SpeechDetector::Open() noexcept {
  if (snowboy_ != nullptr || vad_ != nullptr) {
    return Fail("speech detector is already open");
  }
  // 模型加载留在启动阶段；输入已完成 3A 处理，Snowboy 增益保持 1.0。
  if (boompi_snowboy_legacy_create(audio::kSnowboyResource, audio::kSnowboyModel,
                                   audio::board::kWakeSensitivity, 1.0F, &snowboy_) == 0) {
    Close();
    return Fail("Snowboy initialization failed");
  }
  vad_ = WebRtcVad_Create();
  // 在启动时检查固定的 16 kHz / 320 样本契约，避免逐帧运行后才发现格式不兼容。
  if (vad_ == nullptr || WebRtcVad_Init(vad_) != 0 || WebRtcVad_set_mode(vad_, kVadMode) != 0 ||
      WebRtcVad_ValidRateAndFrameLength(16000, audio::kVoiceFrameSamples16k) != 0) {
    Close();
    return Fail("WebRTC VAD initialization failed");
  }
  error_ = "";
  return true;
}

bool SpeechDetector::ResetVad() noexcept {
  // 抑制窗结束时一起清库内历史和外部计数，不能让被屏蔽的回声续成一句话。
  if (vad_ == nullptr || WebRtcVad_Init(vad_) != 0 || WebRtcVad_set_mode(vad_, kVadMode) != 0) {
    return false;
  }
  vad_speech_ms_ = vad_silence_ms_ = 0U;
  vad_in_speech_ = false;
  return true;
}

bool SpeechDetector::ResetListener() noexcept {
  if (snowboy_ == nullptr || !boompi_snowboy_legacy_reset(snowboy_) || !ResetVad()) {
    return Fail("listener reset failed");
  }
  return true;
}

void SpeechDetector::ArmPlayback() noexcept {
  // 新一轮不继承旧轮次的尾音窗口；先等待渲染与参考，首播蓄水不消耗预热帧数。
  playback_session_active_ = true;
  aec_warmup_armed_ = true;
  silent_playback_bypass_ = false;
  suppress_remaining_ = 0U;
  reference_wait_frames_ = 0U;
}

void SpeechDetector::OnDiscontinuity() noexcept {
  // 采集断点使 3A 的收敛历史失效；这里只重新启用活动轮次的参考等待。
  // 重采样、3A 和检测历史的实际复位由后端顺序完成。
  if (playback_session_active_) {
    aec_warmup_armed_ = true;
    suppress_remaining_ = 0U;
  }
}

bool SpeechDetector::Detect(const audio::CleanAudioFrame& clean,
                            audio::CaptureFrame* frame) noexcept {
  if (frame == nullptr || snowboy_ == nullptr || vad_ == nullptr) {
    return Fail("speech detector is not ready");
  }
  // 输出缓冲逐帧复用，先清掉旧事件和序号；PCM 与其元数据已经在 3A 中一起延迟。
  *frame = {};
  frame->pcm = clean.pcm;
  frame->timestamp_us = clean.metadata.timestamp_us;
  frame->input_dbfs = clean.metadata.input_dbfs;
  frame->reference_active = clean.metadata.reference_active;
  // 去除直流偏置后的 3A 输出电平供上层确认近讲，不替代原始麦的开口门限。
  frame->voice_dbfs = AcRmsDbfs(clean.pcm);

  std::int32_t detection = 0;
  if (!boompi_snowboy_legacy_process_s16(snowboy_, clean.pcm.data(), clean.pcm.size(),
                                         &detection)) {
    return Fail("Snowboy processing failed");
  }
  // 正值为命中的热词编号；VAD 另行处理同一帧，不依赖是否命中唤醒词。
  frame->wake = detection > 0;
  const int speech = WebRtcVad_Process(vad_, 16000, clean.pcm.data(), clean.pcm.size());
  if (speech < 0) {
    return Fail("WebRTC VAD processing failed");
  }
  // 原始麦电平只控制开口准入；一句话开始后仍保留较轻的尾字。
  const bool admitted =
      vad_in_speech_ || frame->input_dbfs >= audio::board::kSpeechAdmissionDbfs;
  frame->vad_now = speech == 1 && admitted;
  // 两个计数只累计连续帧，状态反转即清零另一侧，饱和后不再增长。
  if (frame->vad_now) {
    vad_speech_ms_ = std::min(vad_speech_ms_ + kFrameMs, kVadStartMs);
    vad_silence_ms_ = 0U;
  } else {
    vad_silence_ms_ = std::min(vad_silence_ms_ + kFrameMs, kVadEndMs);
    vad_speech_ms_ = 0U;
  }
  // 起止标志只在跨过门限的当帧置位；vad_now 仍保留未经起止去抖的逐帧判定。
  if (!vad_in_speech_ && vad_speech_ms_ >= kVadStartMs) {
    vad_in_speech_ = true;
    frame->vad_started = true;
  } else if (vad_in_speech_ && vad_silence_ms_ >= kVadEndMs) {
    vad_in_speech_ = false;
    frame->vad_ended = true;
  }
  return true;
}

bool SpeechDetector::GateNearVoice(const PlaybackState& playback,
                                   audio::CaptureFrame* frame) noexcept {
  if (frame == nullptr || vad_ == nullptr) {
    return Fail("speech detector is not ready");
  }
  bool suppress = false;
  // 优先处理一次性的结束事件，避免同帧的旧渲染标志重新开启预热。
  if (playback.end != PlaybackEnd::None) {
    playback_session_active_ = false;
    aec_warmup_armed_ = false;
    silent_playback_bypass_ = false;
    // 主动打断保留已确认的近讲VAD；自然播完才隔离扬声器与房间尾音。
    if (playback.end == PlaybackEnd::Interrupted) {
      suppress_remaining_ = 0U;
    } else {
      suppress_remaining_ = kAecTailFrames;
      suppress = true;
    }
  } else if (silent_playback_bypass_ && playback.output_audible) {
    // 静音期间恢复音量后，重新等待真实参考，不能沿用无回声源时的旁路。
    silent_playback_bypass_ = false;
    aec_warmup_armed_ = true;
    reference_wait_frames_ = 0U;
    suppress = true;
  } else if (aec_warmup_armed_) {
    suppress = true;
    if (!playback.render_started) {
      reference_wait_frames_ = 0U;
    } else if (!playback.output_audible) {
      // 将零增益渲染视为静音，暂时跳过参考等待，允许输出近讲候选。
      aec_warmup_armed_ = false;
      silent_playback_bypass_ = true;
      reference_wait_frames_ = 0U;
      suppress = false;
    } else if (frame->reference_active) {
      // 渲染先于 ALSA 写入发布，必须等采集参考出现，才能开始计算收敛保护时间。
      aec_warmup_armed_ = false;
      suppress_remaining_ = kAecWarmupFrames;
      reference_wait_frames_ = 0U;
    } else if (++reference_wait_frames_ == 50U) {
      // 50 个 20 ms 帧仅触发告警；缺参考时继续抑制，不因等待超时而放行回声。
      std::fprintf(stderr,
                   "boompi-client: warning: AEC reference absent for 1000 ms of playback\n");
    }
  }
  // 窗口包含触发当帧，倒数最后一帧也不放行；清历史后从下一帧重新积累 VAD。
  if (suppress_remaining_ != 0U) {
    suppress = true;
    if (--suppress_remaining_ == 0U && !ResetVad()) {
      return Fail("playback VAD reset failed");
    }
  }
  // 仅输出近讲候选，不改写本帧唤醒与语句边沿，也不在这里确认或执行打断。
  frame->near_voice = !suppress && frame->vad_now;
  return true;
}

void SpeechDetector::Close() noexcept {
  if (snowboy_ != nullptr) {
    boompi_snowboy_legacy_destroy(snowboy_);
  }
  if (vad_ != nullptr) {
    WebRtcVad_Free(vad_);
  }
  snowboy_ = nullptr;
  vad_ = nullptr;
  vad_speech_ms_ = vad_silence_ms_ = 0U;
  suppress_remaining_ = 0U;
  reference_wait_frames_ = 0U;
  vad_in_speech_ = false;
  playback_session_active_ = aec_warmup_armed_ = silent_playback_bypass_ = false;
  error_ = "";
}

}  // namespace boompi::platform::rv1106
