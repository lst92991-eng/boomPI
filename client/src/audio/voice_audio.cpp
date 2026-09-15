/**
 * @file voice_audio.cpp
 * @brief 整理录音片段、缓存句首，并在播放中确认近讲打断。
 *
 * 本文件运行在 应用主线程，AudioTasks 才持有 capture/playback 线程。
 * 正常提问：ProcessEvents → ReadAndProcessCaptureFrame → ProcessListeningFrame → SpeechStart →
 * 缓存/实时 Pcm； 播放插话：ProcessBargeFrame → ConfirmBarge → Barge → 缓存/实时
 * Pcm。两条路径都会先发 开始事件，使 应用模块 有机会分配新 generation，再把 PCM 交给
 * VoiceLink。 下行则由 Play 校验轮次和帧边界，经引擎入队，最终在 ProcessEvents 中观察
 * PlaybackDone。
 */
#include "boompi/audio/voice_audio.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <new>
#include <string>
#include <thread>

#include "board_voice_profile.h"
#include "boompi/audio/audio_tasks.h"
#include "frame_queue.h"

namespace boompi::audio {
namespace {

// 检测确认发生在开口之后；500 ms 历史用于补回确认门限之前的句首。
constexpr std::size_t kPreRollFrames = VoiceFrameContract::FramesForMs(500U);
constexpr std::size_t kBargeHistoryFrames = 32U;
constexpr unsigned kVadStartFrames =
    static_cast<unsigned>(VoiceFrameContract::FramesForMs(120U));
constexpr unsigned kFollowUpFrames =
    static_cast<unsigned>(VoiceFrameContract::FramesForMs(400U));
constexpr unsigned kBargeCandidateFrames = kVadStartFrames;
constexpr unsigned kBargeReferenceLowFrames =
    static_cast<unsigned>(VoiceFrameContract::FramesForMs(60U));
constexpr unsigned kBargeReferenceWaitFrames =
    static_cast<unsigned>(VoiceFrameContract::FramesForMs(300U));
constexpr unsigned kBargeEchoClearFrames =
    static_cast<unsigned>(VoiceFrameContract::FramesForMs(60U));
constexpr unsigned kBargeConfirmFrames =
    static_cast<unsigned>(VoiceFrameContract::FramesForMs(60U));
constexpr unsigned kBargeRetryCooldownFrames =
    static_cast<unsigned>(VoiceFrameContract::FramesForMs(300U));
constexpr std::size_t kMaximumBargeProbeFrames = kBargeCandidateFrames +
                                                 kBargeReferenceWaitFrames +
                                                 kBargeEchoClearFrames + kBargeConfirmFrames;
// 一次结果最多包含一段历史 PCM、开始事件和播放完成事件。
constexpr std::size_t kMaximumEvents = kBargeHistoryFrames + 2U;
constexpr unsigned kCaptureDrainLimit = 8U;
constexpr auto kPlaybackStopWait = std::chrono::milliseconds(60);

static_assert(kPreRollFrames <= kBargeHistoryFrames,
              "barge history must also hold normal pre-roll");
static_assert(kMaximumBargeProbeFrames <= kBargeHistoryFrames,
              "barge history must hold one complete acoustic probe");
static_assert(kVoiceFrameSamples == 320U && kTtsFrameSamples == 320U,
              "VoiceAudio public and wire frame contracts changed");

}  // namespace

/** @brief actor 独占的语句整理状态；跨线程工作只经 tasks 的同步接口交接。 */
struct VoiceAudio::Impl final {
  // 描述是否接纳、如何整理输入；对话 Offline/Waiting/Speaking 等六状态仍在 应用模块。
  enum class InputState : std::uint8_t {
    Idle,
    Listening,
    FollowingUp,
    Capturing,
  };
  // 已有近讲候选后依次等待硬件参考降下、房间尾音消退、再次确认人声。
  enum class BargeStage : std::uint8_t {
    WaitCandidate,
    WaitReferenceLow,
    WaitEchoTail,
    ConfirmNearSpeech,
  };

  AudioTasks tasks{};
  // 仅保留尚未准入的滚动录音；处理结果直接写入调用方的 events。
  FrameQueue<CaptureFrame, kBargeHistoryFrames> history;
  InputState input_state{InputState::Idle};
  BargeStage barge_stage{BargeStage::WaitCandidate};
  unsigned follow_up_frames{0U};
  unsigned barge_stage_frames{0U};
  unsigned barge_reference_low_frames{0U};
  unsigned barge_cooldown_frames{0U};
  // 仅记住当前下行归属以生成 Barge/PlaybackDone；新 generation 只由 应用模块 分配。
  std::uint32_t playback_generation{0U};
  bool playback_ending{false};
  bool open{false};
  bool fatal{false};
  std::array<char, 192U> error{};

  /** @brief 先隔离采集断点，再按是否有活动播放选择插话探测或正常语句整理。 */
  void ProcessCaptureFrame(const CaptureFrame& frame,
                           std::vector<AudioEvent>& events) noexcept {
    if (frame.discontinuity) {
      ReportFault(
          events,
          frame.actor_overrun ? "voice capture queue overrun" : "voice capture discontinuity",
          false);
      return;
    }

    if (playback_generation != 0U) {
      const std::size_t history_limit =
          barge_stage == BargeStage::WaitCandidate ? kPreRollFrames : kBargeHistoryFrames;
      SaveHistory(frame, history_limit);
      ProcessBargeFrame(frame, events);
      return;
    }

    ProcessListeningFrame(frame, events);
  }

  /// 正常输入只整理句首、连续PCM和句尾；播放期间的插话控制在上一步单独处理。
  void ProcessListeningFrame(const CaptureFrame& frame,
                             std::vector<AudioEvent>& events) noexcept {
    switch (input_state) {
      case InputState::Idle:
        if (frame.wake) {
          AudioEvent wake{};
          wake.kind = AudioEventKind::Wake;
          events.push_back(wake);
        }
        return;

      case InputState::Listening:
        SaveHistory(frame, kPreRollFrames);
        if (frame.vad_started) {
          EmitBufferedSpeech(events, AudioEventKind::SpeechStart, 0U);
        }
        return;

      case InputState::FollowingUp:
        // 未达到 400 ms 追问准入的短句已经结束后，旧 END 不能留在下一句话的
        // pre-roll 中；否则真正问题刚被准入就会被旧边界提前截断。
        if (frame.vad_ended) {
          follow_up_frames = 0U;
          history.Clear();
          return;
        }
        SaveHistory(frame, kPreRollFrames);
        if (!frame.near_voice) {
          follow_up_frames = 0U;
        } else if (++follow_up_frames >= kFollowUpFrames) {
          EmitBufferedSpeech(events, AudioEventKind::SpeechStart, 0U);
        }
        return;

      case InputState::Capturing:
        // 句尾也交付完整 PCM；应用发送这帧后再结束上行。
        events.push_back(PcmEvent(frame, frame.vad_ended));
        if (frame.vad_ended) {
          DisarmInput();
        }
        return;
    }
  }

  /**
   * @brief 候选人声触发短暂静音，参考和房间尾音消退后再确认打断。
   * 路径为 120 ms 连续候选 → 最多等参考 300 ms（需连续低 60 ms）→ 尾音清理
   * 60 ms → 连续近讲 60 ms。计数来自已处理的 20 ms 帧，不是墙上时钟。
   */
  void ProcessBargeFrame(const CaptureFrame& frame, std::vector<AudioEvent>& events) noexcept {
    switch (barge_stage) {
      case BargeStage::WaitCandidate:
        if (barge_cooldown_frames != 0U) {
          --barge_cooldown_frames;
          return;
        }
        if (!IsBargeVoice(frame)) {
          barge_stage_frames = 0U;
          return;
        }
        if (++barge_stage_frames < kBargeCandidateFrames) {
          return;
        }
        KeepNewestHistory(kBargeCandidateFrames);
        barge_stage_frames = 0U;
        barge_reference_low_frames = 0U;
        barge_stage = BargeStage::WaitReferenceLow;
        // 只让随后渲染的 PCM 变为零；ALSA 中已有声音还会继续，所以必须观察硬件参考。
        tasks.SetPlaybackScale(0.0F);
        return;

      case BargeStage::WaitReferenceLow:
        if (++barge_stage_frames > kBargeReferenceWaitFrames) {
          RejectBarge();
          return;
        }
        if (frame.reference_active) {
          barge_reference_low_frames = 0U;
        } else {
          ++barge_reference_low_frames;
        }
        if (barge_reference_low_frames < kBargeReferenceLowFrames) {
          return;
        }
        barge_stage_frames = 0U;
        barge_stage = BargeStage::WaitEchoTail;
        return;

      case BargeStage::WaitEchoTail:
        if (frame.reference_active) {
          RejectBarge();
          return;
        }
        if (++barge_stage_frames < kBargeEchoClearFrames) {
          return;
        }
        barge_stage_frames = 0U;
        barge_stage = BargeStage::ConfirmNearSpeech;
        return;

      case BargeStage::ConfirmNearSpeech:
        if (frame.reference_active || !IsBargeVoice(frame)) {
          RejectBarge();
          return;
        }
        if (++barge_stage_frames >= kBargeConfirmFrames) {
          ConfirmBarge(events);
        }
        return;
    }
  }

  /**
   * @brief 将已准入的历史一次排成「开始事件、PCM…、可选末帧」的连续事件序列。
   * 开始事件与句首一起交付；遇到第一个 VAD END 即停止，
   * 防止探测期间已结束的短句与后续背景声拼接。无 END 时切到实时录音继续追加。
   */
  void EmitBufferedSpeech(std::vector<AudioEvent>& events, const AudioEventKind start_kind,
                          const std::uint32_t generation) noexcept {
    AudioEvent start{};
    start.kind = start_kind;
    start.generation = generation;
    events.push_back(start);
    CaptureFrame frame;
    input_state = InputState::Capturing;
    while (history.Pop(&frame)) {
      events.push_back(PcmEvent(frame, frame.vad_ended));
      if (frame.vad_ended) {
        DisarmInput();
        break;
      }
    }
  }

  /**
   * @brief 停止旧播放并先交付 Barge，再交付探测期间保留的人声。
   * Barge 携带旧 generation，应用模块 收到后分配新轮次并发送 START|SUPERSEDE；
   * 单独 DropPlayback 只解决本地出声，不能替代服务端旧回复的退休。
   */
  void ConfirmBarge(std::vector<AudioEvent>& events) noexcept {
    const std::uint32_t interrupted_generation = playback_generation;
    tasks.DropPlayback();
    playback_generation = 0U;
    playback_ending = false;
    ResetBargeProbe();
    barge_cooldown_frames = 0U;
    EmitBufferedSpeech(events, AudioEventKind::Barge, interrupted_generation);
  }

  /**
   * @brief 将播放线程的原子完成快照变成有 generation 的 actor 事件。
   * 只有已接收 end 且引擎无故障才是自然完成；首播后无 end 却完成意味着异常。
   */
  void CheckPlaybackCompletion(std::vector<AudioEvent>& events) noexcept {
    if (playback_generation == 0U || !tasks.IsPlaybackDone()) {
      return;
    }
    const std::uint32_t generation = playback_generation;
    playback_generation = 0U;
    const bool expected = playback_ending;
    playback_ending = false;
    ResetBargeProbe();
    barge_cooldown_frames = 0U;
    history.Clear();
    if (tasks.HasPlaybackFailed() || !expected) {
      ReportFault(events, "voice playback failed", true, generation);
      return;
    }
    AudioEvent done{};
    done.kind = AudioEventKind::PlaybackDone;
    done.generation = generation;
    events.push_back(done);
  }

  /// 取一帧前端结果；先观察旧播放完成，使同帧录音不会继续归到已结束的播放探测。
  CaptureResult ReadAndProcessCaptureFrame(std::vector<AudioEvent>& events,
                                           const std::chrono::milliseconds timeout) noexcept {
    CaptureFrame frame{};
    const CaptureResult result = tasks.ReadProcessedFrame(&frame, timeout);
    if (result == CaptureResult::Frame) {
      CheckPlaybackCompletion(events);
      if (!fatal) {
        ProcessCaptureFrame(frame, events);
      }
    } else if (result == CaptureResult::Failed) {
      ReportFault(events, "voice capture failed", true);
    }
    return result;
  }

  /**
   * @brief 首包开始前等待上一轮 drop 收尾，最多观察 60 ms，并继续排空已就绪采集。
   * 这不是新的网络超时；失败交给 actor 撤回该回复，不在这里无限等待硬件。
   */
  bool WaitForPlaybackStop() {
    const auto deadline = std::chrono::steady_clock::now() + kPlaybackStopWait;
    CaptureFrame frame{};
    while (!tasks.IsPlaybackDone()) {
      for (unsigned i = 0; i < kCaptureDrainLimit; ++i) {
        // 首个回复包只等待旧播放停止；排空录音即可，不生成随后会被丢弃的PCM事件副本。
        const CaptureResult result =
            tasks.ReadProcessedFrame(&frame, std::chrono::milliseconds::zero());
        if (result == CaptureResult::Failed ||
            (result == CaptureResult::Frame && frame.discontinuity)) {
          fatal = fatal || result == CaptureResult::Failed;
          SetError(result == CaptureResult::Failed ? "voice capture failed"
                   : frame.actor_overrun           ? "voice capture queue overrun"
                                                   : "voice capture discontinuity");
          DisarmInput();
          ResetBargeProbe();
          return false;
        }
        if (result == CaptureResult::Timeout) {
          break;
        }
      }
      if (fatal || std::chrono::steady_clock::now() >= deadline) {
        return false;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
  }

  /** @brief 保存尚未提交的最近 limit 帧；满时淘汰最老历史，正常 PCM 本轮结果不采用此语义。 */
  void SaveHistory(const CaptureFrame& frame, const std::size_t limit) noexcept {
    // 只有尚未准入的历史允许淘汰；已准入的 PCM 直接交给调用方。
    while (history.Size() >= limit) {
      static_cast<void>(history.Pop());
    }
    static_cast<void>(history.Push(frame));
  }

  /// 打断候选确认后仅保留候选人声起点，后面再追加静音探测期间的 PCM。
  void KeepNewestHistory(const std::size_t count) noexcept {
    while (history.Size() > count) {
      static_cast<void>(history.Pop());
    }
  }

  /// 把内部采集帧转换为上行事件，不向 application 暴露 dBFS、硬件参考和检测历史。
  AudioEvent PcmEvent(const CaptureFrame& frame, const bool end) const noexcept {
    AudioEvent event{};
    event.kind = AudioEventKind::Pcm;
    event.pcm = frame.pcm;
    event.sequence = frame.sequence;
    event.timestamp_us = frame.timestamp_us;
    event.end = end;
    return event;
  }

  /// near_voice 已经过播放保护，再叠加 3A 后电平门限；原始麦电平不用作近讲确认。
  bool IsBargeVoice(const CaptureFrame& frame) const noexcept {
    return frame.near_voice && frame.voice_dbfs >= board::kBargeVoiceDbfs;
  }

  /// 候选消失或参考未及时退去时恢复播放，并冷却 300 ms，避免连续静音探测。
  void RejectBarge() noexcept {
    ResetBargeProbe();
    barge_cooldown_frames = kBargeRetryCooldownFrames;
  }

  /** @brief 结束探测并恢复用户原音量的乘数；不改变用户保存的音量值。 */
  void ResetBargeProbe() noexcept {
    barge_stage = BargeStage::WaitCandidate;
    barge_stage_frames = 0U;
    barge_reference_low_frames = 0U;
    tasks.SetPlaybackScale(1.0F);
  }

  /// 退出录音准入并清除句首；ALSA 继续采集，空闲帧仍可触发唤醒。
  void DisarmInput() noexcept {
    input_state = InputState::Idle;
    follow_up_frames = 0U;
    history.Clear();
  }

  /// 显式停止或下行失败时忘记旧播放归属，防止异步 drop 完成被误报为自然播完。
  void DiscardPlayback() noexcept {
    tasks.DropPlayback();
    playback_generation = 0U;
    playback_ending = false;
    ResetBargeProbe();
    history.Clear();
  }

  /**
   * @brief 将未交付的语句替换为单个故障事件，防止残缺 PCM 继续作为有效输入发送。
   * fatal 一旦置位便保持到 Close/Open；可恢复的缺帧仅由 actor 取消本轮并重新监听。
   * 本函数不直接停止播放或发送 STOP，后续由 App_HandleAudioFault 完成。
   */
  void ReportFault(std::vector<AudioEvent>& events, const char* const why, const bool is_fatal,
                   const std::uint32_t generation = 0U) noexcept {
    SetError(why);
    fatal = fatal || is_fatal;
    DisarmInput();
    ResetBargeProbe();
    events.clear();
    AudioEvent event{};
    event.kind = AudioEventKind::Fault;
    event.generation = generation != 0U ? generation : playback_generation;
    events.push_back(event);
  }

  void SetError(const char* const why) noexcept {
    std::snprintf(error.data(), error.size(), "%s", why);
  }

  void ClearError() noexcept {
    error.fill('\0');
  }
};

VoiceAudio::~VoiceAudio() noexcept {
  Close();
  delete impl_;
}

bool VoiceAudio::Open(const std::uint8_t volume) {
  if (impl_ == nullptr) {
    impl_ = new (std::nothrow) Impl;
  }
  if (impl_ == nullptr) {
    return false;
  }
  Close();
  impl_->history.Clear();
  impl_->fatal = false;
  impl_->ClearError();
  const float playback_gain = static_cast<float>(std::min<std::uint8_t>(volume, 100U)) / 100.0F;
  if (!impl_->tasks.Start(playback_gain)) {
    impl_->fatal = true;
    const std::string error = impl_->tasks.LastError();
    impl_->SetError(error.c_str());
    return false;
  }
  impl_->open = true;
  return true;
}

void VoiceAudio::ProcessEvents(std::vector<AudioEvent>& events,
                               const std::chrono::milliseconds timeout) {
  events.clear();
  if (impl_ == nullptr || !impl_->open || timeout < std::chrono::milliseconds::zero()) {
    return;
  }
  // 只在应用线程分配一次容量，后续每轮复用；实时采集和播放线程不操作这个容器。
  events.reserve(kMaximumEvents);
  impl_->CheckPlaybackCompletion(events);
  for (unsigned count = 0; count < kCaptureDrainLimit && events.empty(); ++count) {
    const auto wait = count == 0 ? timeout : std::chrono::milliseconds::zero();
    if (impl_->ReadAndProcessCaptureFrame(events, wait) != CaptureResult::Frame) {
      break;
    }
  }
  impl_->CheckPlaybackCompletion(events);
}

bool VoiceAudio::Listen(const ListenMode mode) {
  if (impl_ == nullptr || !impl_->open || impl_->fatal || impl_->playback_generation != 0U) {
    return false;
  }
  impl_->DisarmInput();
  impl_->ResetBargeProbe();
  impl_->barge_cooldown_frames = 0U;
  if (!impl_->tasks.ResetListener()) {
    impl_->fatal = true;
    impl_->SetError("voice listener reset failed");
    return false;
  }
  impl_->ClearError();
  impl_->input_state = mode == ListenMode::FollowUp ? Impl::InputState::FollowingUp
                                                    : Impl::InputState::Listening;
  return true;
}

bool VoiceAudio::Play(const std::uint32_t generation, const std::uint8_t* const pcm,
                      const std::size_t bytes, const std::uint32_t sequence, const bool start,
                      const bool end) {
  if (impl_ == nullptr || !impl_->open || impl_->fatal || generation == 0U) {
    return false;
  }
  const bool invalid_pcm =
      pcm == nullptr || bytes == 0U || bytes > kTtsFrameSamples * 2U || (bytes & 1U) != 0U;
  const bool invalid_frame =
      (!end && bytes != kTtsFrameSamples * 2U) || (start != (sequence == 0U));
  if (invalid_pcm || invalid_frame) {
    return false;
  }

  if (start) {
    // 等旧 drop 完成后先武装采集侧 AEC，再宣布新播放归属，最后才允许 PCM 入队。
    if (impl_->playback_generation != 0U || !impl_->WaitForPlaybackStop() ||
        !impl_->tasks.BeginPlayback()) {
      impl_->SetError("voice playback start failed");
      return false;
    }
    impl_->playback_generation = generation;
    impl_->playback_ending = false;
    impl_->ResetBargeProbe();
    impl_->barge_cooldown_frames = 0U;
    impl_->history.Clear();
  } else if (generation != impl_->playback_generation || impl_->playback_ending) {
    return false;
  }

  const QueueTtsResult queued = impl_->tasks.QueueReplyFrame(pcm, bytes, sequence);
  if (queued != QueueTtsResult::Queued) {
    // 满队列、序号缺口等都终止本轮；这里不跳过一包后继续播，从而掩盖网络音频缺失。
    impl_->SetError("voice playback queue rejected PCM");
    impl_->DiscardPlayback();
    return false;
  }
  if (end) {
    // END 只改变生产状态，剩余缓冲和 ALSA drain 仍由播放线程消费完成。
    if (!impl_->tasks.EndPlayback()) {
      impl_->SetError("voice playback finish failed");
      impl_->DiscardPlayback();
      return false;
    }
    impl_->playback_ending = true;
  }
  impl_->ClearError();
  return true;
}

void VoiceAudio::StopPlayback() {
  if (impl_ == nullptr || !impl_->open) {
    return;
  }
  impl_->DiscardPlayback();
  impl_->barge_cooldown_frames = 0U;
}

void VoiceAudio::CancelInput() {
  if (impl_ == nullptr || !impl_->open) {
    return;
  }
  impl_->DisarmInput();
  // 上行取消不影响正在播放的回复，后续采集帧仍可检测近讲打断。
  impl_->ResetBargeProbe();
  impl_->barge_cooldown_frames = 0U;
}

void VoiceAudio::SetVolume(const std::uint8_t volume) {
  if (impl_ == nullptr || !impl_->open) {
    return;
  }
  impl_->tasks.SetPlaybackGain(static_cast<float>(std::min<std::uint8_t>(volume, 100U)) /
                               100.0F);
}

bool VoiceAudio::IsHealthy() const {
  return impl_ != nullptr && impl_->open && !impl_->fatal;
}

std::string VoiceAudio::LastError() const {
  if (impl_ == nullptr) {
    return "voice audio is not allocated";
  }
  if (impl_->error[0] != '\0') {
    return impl_->error.data();
  }
  return impl_->tasks.LastError();
}

void VoiceAudio::Close() noexcept {
  if (impl_ == nullptr) {
    return;
  }
  impl_->tasks.Stop();
  impl_->history.Clear();
  impl_->input_state = Impl::InputState::Idle;
  impl_->barge_stage = Impl::BargeStage::WaitCandidate;
  impl_->follow_up_frames = impl_->barge_stage_frames = impl_->barge_reference_low_frames =
      impl_->barge_cooldown_frames = 0U;
  impl_->playback_generation = 0U;
  impl_->playback_ending = false;
  impl_->open = false;
  impl_->fatal = false;
}

}  // namespace boompi::audio
