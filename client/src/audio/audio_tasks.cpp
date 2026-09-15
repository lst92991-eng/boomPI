/**
 * @file audio_tasks.cpp
 * @brief 采集线程持续产帧，播放线程消费 TTS，actor 通过队列和命令连接两者。
 *
 * 启动链：Start → AudioPipeline::Open → ReadMicrophoneTask/PlaySpeakerTask。
 * 输入链：读取四通道 → 后端 3A/检测 → SendCaptureToQueue → ReadProcessedFrame → VoiceAudio。
 * 输出链：BeginPlayback 的采集握手 → QueueReplyFrame → PlaybackReady → Render20ms →
 * FinishPlayback。Stop 发布停止、解除 ALSA 阻塞、join 后才释放后端资源。
 */
#include "boompi/audio/audio_tasks.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <new>
#include <thread>

#include "audio_pipeline.h"
#include "audio_thread.h"
#include "frame_queue.h"

namespace boompi::audio {
namespace {
// 上下行统一16kHz，每个完整PCM包为320样本；声卡仍保持已验证的48kHz。
constexpr std::size_t kTtsSlots = VoiceFrameContract::FramesForMs(1500U);
constexpr std::size_t kInitialTtsSlots = VoiceFrameContract::FramesForMs(180U);
constexpr std::size_t kRebufferTtsSlots = VoiceFrameContract::FramesForMs(40U);
// 短于 30 ms 的到包间隔波动由 ALSA 缓冲吸收，避免频繁停播造成周期性卡顿。
constexpr auto kRebufferGrace = std::chrono::milliseconds(30);
constexpr std::size_t kCaptureSlots = VoiceFrameContract::FramesForMs(80U);
// 控制命令必须快速跨越下一个 20 ms 帧边界；100 ms 允许少量调度抖动并限制关停等待。
constexpr auto kCaptureCommandTimeout = std::chrono::milliseconds(100);
// capture 优先级更高，因为丢失硬件输入不可恢复；playback 数据仍可由环形缓冲吸收抖动。
constexpr int kCapturePriority = 40;
constexpr int kPlaybackPriority = 30;

}  // namespace

/** @brief 线程、环形缓冲与同步状态的共同生命周期；各字段按下方锁归属访问。 */
struct AudioTasks::Impl final {
  // 一个 slot 对应 20 ms。EOS 允许最后一个 slot 只使用前 `used` 个 sample。
  struct TtsSlot final {
    std::array<std::int16_t, kTtsFrameSamples> pcm{};
    std::size_t used{0U};
  };
  // 控制命令由 application 发出，在 capture period 边界由 capture_thread 执行。
  // 这条单槽握手保证 Snowboy/VAD/后端状态始终只有采集线程写入。
  enum class CaptureCommand : std::uint8_t { None, ResetListener, ArmPlayback };

  // capture_thread 独占采集/3A，并在采集帧边界执行后端控制命令；
  // BeginPlayback 完成该握手后，playback_thread 独占播放热路径。
  platform::rv1106::AudioPipeline pipeline{};
  std::thread capture_thread{}, playback_thread{};
  // mutex 保护 TTS/播放状态；capture_mutex 保护采集队列和命令；error_mutex 仅保护错误文本。
  mutable std::mutex mutex{}, capture_mutex{}, error_mutex{};
  std::condition_variable condition{}, capture_condition{};
  std::array<char, 192U> error{};
  // capture 是单生产者/单消费者有界环。满时清环并发布一个显式 discontinuity 帧。
  FrameQueue<CaptureFrame, kCaptureSlots> capture;
  // 原始四通道帧较大，随Impl预分配，不占用实时线程的大块局部栈空间。
  RawCaptureFrame raw_capture{};
  CaptureCommand capture_command{CaptureCommand::None};
  bool command_succeeded{false};
  FrameQueue<TtsSlot, kTtsSlots> tts;
  // capture_sequence 描述本地处理时间线；next_tts_sequence 检查服务端下行包是否连续。
  std::uint64_t capture_sequence{0U}, next_tts_sequence{0U};
  bool open{false}, capture_failed{false};
  // `active` 覆盖 Begin 到 drain/drop；`ending` 表示网络已声明没有后续 PCM。
  bool active{false}, ending{false}, playback_started{false}, rebuffering{false};
  bool drop{false};
  // stop 跨两条线程发布生命周期；gain/scale 和完成状态允许 UI/application 无锁读取。
  std::atomic<bool> stop{false};
  std::atomic<float> playback_gain{1.0F}, playback_scale{1.0F};
  std::atomic<bool> is_done{true}, playback_failed{false};

  // 采集任务：读麦克风 → 重采样/3A/检测 → 放入采集队列。
  void ReadMicrophoneTask() noexcept {
    SetAudioThreadPriority("boompi-capture", kCapturePriority);
    while (!stop.load()) {
      // 1. 在下一帧开始前处理检测器复位或播放武装请求。
      if (!ApplyCaptureCommand()) {
        FailCapture();
        break;
      }
      if (stop.load()) {
        break;
      }

      // 2. 读取声卡四通道的 20 ms 原始声音。
      const bool read = pipeline.ReadCapture20ms(&raw_capture);
      if (stop.load()) {
        break;  // Stop 中断硬件等待属于正常退出。
      }

      // 3. 把原始声音处理为 16 kHz 单声道，附上唤醒与 VAD 结果。
      CaptureFrame frame;
      const bool processed = read && pipeline.ProcessCapture20ms(raw_capture, &frame);
      if (stop.load()) {
        break;
      }
      if (!processed) {
        FailCapture();
        break;
      }

      // 4. 交给主线程中的语句整理，唤醒等待取帧的一方。
      SendCaptureToQueue(frame);
      capture_condition.notify_one();
    }
  }

  // 播放任务：等回复帧 → 取出一帧 → 重采样/音量/写声卡 → 尾播收尾。
  void PlaySpeakerTask() noexcept {
    SetAudioThreadPriority("boompi-playback", kPlaybackPriority);
    while (true) {
      std::unique_lock<std::mutex> lock(mutex);

      // 1. 等待起播缓存；持续播放只在缺包超过 30 ms 后重新蓄水。
      if (NeedsRebufferWait() && !condition.wait_for(lock, kRebufferGrace, [this] {
            return stop.load() || drop || ending || tts.Size() != 0U;
          })) {
        rebuffering = true;
      }
      condition.wait(lock, [this] {
        return stop.load() || drop || PlaybackReady();
      });
      if (stop.load()) {
        break;
      }
      if (drop) {
        lock.unlock();
        FinishPlayback(true, false);
        continue;
      }

      // 2. 首次出声前准备声卡；持锁防止准备过程清掉刚到达的停止请求。
      if (!playback_started && !pipeline.PreparePlayback()) {
        lock.unlock();
        FinishPlayback(false, true);
        continue;
      }
      TtsSlot frame;
      if (!tts.Pop(&frame)) {
        lock.unlock();
        FinishPlayback(false, false);  // END 已到且队列为空，只剩声卡尾播。
        continue;
      }
      playback_started = true;
      rebuffering = false;
      const bool last_frame = ending && tts.Size() == 0U;
      lock.unlock();

      // 3. 释放队列锁后处理并播放，主线程可以同时提交下一帧或停止。
      const float gain = playback_gain.load() * playback_scale.load();
      const bool played = pipeline.Render20ms(frame.pcm.data(), frame.used, gain);

      // 4. 最后一帧等待声卡播完；失败或插话则丢弃剩余声音。
      if (last_frame || !played) {
        FinishPlayback(false, !played);
      }
    }
  }

  /**
   * @brief capture 为本地帧编号并复制到 4 槽环；满环显式交付断点而非悄悄覆盖。
   * XRUN 与 actor 落后在这里汇合，VoiceAudio 收到 discontinuity 后终止残缺输入。
   */
  void SendCaptureToQueue(CaptureFrame& frame) noexcept {
    frame.sequence = capture_sequence++;
    std::lock_guard<std::mutex> lock(capture_mutex);
    const bool capture_xrun = frame.discontinuity;
    const bool actor_overrun = capture.Size() == kCaptureSlots;

    // XRUN 和消费者落后都会造成缺帧。丢弃已排队的旧数据，并让新帧携带断点，
    // VoiceAudio 收到后结束当前录音，不能把缺失的音频当作连续语音上传。
    if (capture_xrun || actor_overrun) {
      capture.Clear();
      frame.discontinuity = true;
      // 两个原因同时发生时，对外优先报告硬件 XRUN。
      frame.actor_overrun = actor_overrun && !capture_xrun;
    }
    static_cast<void>(capture.Push(frame));
  }

  /**
   * @brief actor 提交单槽控制并等待采集线程应答，超时转成采集失败。
   * wait_for 会释放 capture_mutex，采集线程可以处理当前 period 并完成命令；
   * notify 本身不能中断 ALSA read，实际执行点始终是下一轮 ReadMicrophoneTask 顶部。
   */
  bool RunCaptureCommand(CaptureCommand command) noexcept {
    // 调用方等待采集线程确认，Snowboy/VAD 始终由采集线程操作。
    std::unique_lock<std::mutex> lock(capture_mutex);
    if (stop.load() || capture_failed || capture_command != CaptureCommand::None) {
      return false;
    }
    capture_command = command;
    capture_condition.notify_all();
    if (!capture_condition.wait_for(lock, kCaptureCommandTimeout, [this] {
          return stop.load() || capture_failed || capture_command == CaptureCommand::None;
        })) {
      // ALSA 没有交出下一个 period 时，命令无法安全越过帧边界执行。明确失败
      // 可让 application 退出并由 Close 中断 readi，不能把停止请求永久卡在这里。
      capture_failed = true;
      SetError("capture control timed out");
      capture_condition.notify_all();
      return false;
    }
    return !stop.load() && !capture_failed && capture_command == CaptureCommand::None &&
           command_succeeded;
  }

  /** @brief capture 线程在读取下一 period 前取命令、执行后端操作并发布应答。 */
  bool ApplyCaptureCommand() noexcept {
    // 后端调用不能持有 capture_mutex，否则 application 无法观察停止和命令完成。
    CaptureCommand command = CaptureCommand::None;
    {
      std::lock_guard<std::mutex> lock(capture_mutex);
      if (capture_failed) {
        return false;
      }
      if (capture_command == CaptureCommand::None) {
        return true;
      }
      command = capture_command;
    }
    bool succeeded = false;
    switch (command) {
      case CaptureCommand::ResetListener:
        succeeded = pipeline.ResetListener();
        break;
      case CaptureCommand::ArmPlayback:
        succeeded = pipeline.ArmPlayback();
        break;
      case CaptureCommand::None:
        break;
    }
    {
      std::lock_guard<std::mutex> lock(capture_mutex);
      // 已采集 PCM 仍属于时间轴，不能因 listener reset 被静默丢弃；但 reset 前
      // 的 Snowboy/VAD 判定不得穿越状态边界。reference、dBFS 和连续性标记保持原样。
      if (command == CaptureCommand::ResetListener) {
        ClearQueuedListenerDecisions();
      }
      command_succeeded = succeeded;
      capture_command = CaptureCommand::None;
    }
    capture_condition.notify_all();
    return succeeded;
  }

  /** @brief 持 capture_mutex 清除队列中的旧准入结果，不删除 PCM 或掩盖断点。 */
  void ClearQueuedListenerDecisions() noexcept {
    // PCM 和连续性元数据保留在原时间轴，只清除旧监听阶段产生的业务判断。
    for (std::size_t offset = 0U; offset < capture.Size(); ++offset) {
      auto& frame = capture[offset];
      frame.wake = frame.vad_now = frame.vad_started = frame.vad_ended = false;
      frame.near_voice = false;
    }
  }

  /// 调用时持有 mutex。首播蓄满 180 ms，欠载恢复蓄满 40 ms；EOS 允许短尾帧立即通过。
  bool PlaybackReady() const noexcept {
    if (!active) {
      return false;
    }
    if (tts.Size() == 0U) {
      return ending;
    }
    if (ending) {
      return true;
    }
    if (tts[0].used != kTtsFrameSamples) {
      return false;
    }
    // 除末包外每槽都是完整帧，槽数足以决定蓄水，不另维护累计样本数。
    const std::size_t full_slots =
        tts.Size() - (tts[tts.Size() - 1U].used != kTtsFrameSamples ? 1U : 0U);
    if (!playback_started) {
      return full_slots >= kInitialTtsSlots;
    }
    if (rebuffering) {
      return full_slots >= kRebufferTtsSlots;
    }
    return true;
  }

  /// 仅已开始且尚未 END 的空环需要宽限等待；初次蓄水和已经重缓冲不重复计数。
  bool NeedsRebufferWait() const noexcept {
    return active && playback_started && tts.Size() == 0U && !ending && !drop && !rebuffering;
  }

  /**
   * @brief 播放线程完成自然 drain 或取消 drop，然后发布本轮最终结果。
   * ALSA 调用期间不持 mutex，actor 仍可发送取消；is_done 最后发布，使上层看到完成
   * 时已能读取 failure 快照并开始下一次 BeginPlayback。
   */
  void FinishPlayback(bool discard, bool failed) noexcept {
    // drain/write 期间仍可能收到用户 drop；每次阻塞调用返回后必须重新观察取消。
    {
      std::lock_guard<std::mutex> lock(mutex);
      if (drop) {
        discard = true;
        failed = false;
      }
    }
    // 正常 EOS 使用 drain 播完内核缓存；打断或错误使用 drop 立即停止旧回答。
    if (discard || failed) {
      pipeline.DropPlayback();
    } else {
      failed = !pipeline.DrainPlayback();
      if (failed) {
        pipeline.DropPlayback();
      }
    }
    std::lock_guard<std::mutex> lock(mutex);
    // drain期间也可能收到取消。中断返回后再查一次，主动取消不记为播放故障。
    if (drop && !discard) {
      failed = false;
      pipeline.DropPlayback();
    }
    active = ending = drop = playback_started = rebuffering = false;
    ClearPlaybackQueue();
    playback_scale.store(1.0F);
    playback_failed.store(failed);
    is_done.store(true);
    condition.notify_all();
  }

  /// 播放运行期间须持有 mutex；Start/Stop 在线程未访问时也可清理。
  void ClearPlaybackQueue() noexcept {
    tts.Clear();
  }

  /// 采集故障也要唤醒等待命令或新帧的线程，避免它们一直等到超时。
  void FailCapture() noexcept {
    std::lock_guard<std::mutex> lock(capture_mutex);
    capture_failed = true;
    capture_condition.notify_all();
  }

  void SetError(const char* text) noexcept {
    std::lock_guard<std::mutex> lock(error_mutex);
    std::snprintf(error.data(), error.size(), "%s", text);
  }

  void ClearError() noexcept {
    std::lock_guard<std::mutex> lock(error_mutex);
    error.fill('\0');
  }
};

AudioTasks::~AudioTasks() noexcept {
  Stop();
  delete impl_;
}

bool AudioTasks::Start(const float playback_gain) noexcept {
  // 后端先完整就绪，再发布 open 并创建线程，线程启动后不会看到半初始化资源。
  if (impl_ == nullptr) {
    impl_ = new (std::nothrow) Impl;
  }
  if (impl_ == nullptr) {
    return false;
  }
  if (impl_->open) {
    impl_->SetError("audio is already open");
    return false;
  }
  if (!std::isfinite(playback_gain) || playback_gain < 0.0F) {
    impl_->SetError("invalid playback gain configuration");
    return false;
  }
  if (!impl_->pipeline.Open()) {
    return false;
  }
  impl_->playback_gain.store(playback_gain);
  impl_->stop.store(false);
  impl_->capture_failed = false;
  impl_->capture_command = Impl::CaptureCommand::None;
  impl_->command_succeeded = false;
  impl_->capture_sequence = 0U;
  impl_->capture.Clear();
  impl_->open = true;
  try {
    impl_->capture_thread = std::thread(&Impl::ReadMicrophoneTask, impl_);
    impl_->playback_thread = std::thread(&Impl::PlaySpeakerTask, impl_);
  } catch (...) {
    impl_->SetError("audio realtime thread creation failed");
    Stop();
    return false;
  }
  impl_->ClearError();
  return true;
}

CaptureResult AudioTasks::ReadProcessedFrame(CaptureFrame* const frame,
                                             const std::chrono::milliseconds timeout) noexcept {
  // 高优先级采集线程持续排空 ALSA；application actor 只消费已处理的固定帧。
  if (impl_ == nullptr || !impl_->open || frame == nullptr ||
      timeout < std::chrono::milliseconds::zero()) {
    return CaptureResult::Failed;
  }
  std::unique_lock<std::mutex> lock(impl_->capture_mutex);
  if (!impl_->capture_condition.wait_for(lock, timeout, [this] {
        return impl_->stop.load() || impl_->capture_failed || impl_->capture.Size() != 0U;
      })) {
    return CaptureResult::Timeout;
  }
  if (impl_->stop.load() || impl_->capture_failed) {
    return CaptureResult::Failed;
  }
  static_cast<void>(impl_->capture.Pop(frame));
  return CaptureResult::Frame;
}

bool AudioTasks::ResetListener() noexcept {
  if (impl_ == nullptr || !impl_->open || impl_->stop.load()) {
    return false;
  }
  return impl_->RunCaptureCommand(Impl::CaptureCommand::ResetListener);
}

QueueTtsResult AudioTasks::QueueReplyFrame(const std::uint8_t* const bytes,
                                           const std::size_t byte_count,
                                           const std::uint64_t sequence) noexcept {
  if (impl_ == nullptr || !impl_->open) {
    return QueueTtsResult::NotOpen;
  }
  if (bytes == nullptr || byte_count == 0U || byte_count > kTtsFrameSamples * 2U ||
      (byte_count & 1U) != 0U) {
    return QueueTtsResult::InvalidArgument;
  }
  const std::size_t samples = byte_count / 2U;
  // WSS 事件已经回到 actor 线程，此处是 TTS ring 的唯一生产者。
  std::lock_guard<std::mutex> lock(impl_->mutex);
  if (!impl_->active) {
    return QueueTtsResult::NotActive;
  }
  // 短帧只能是最后一包；保留它等待 EndPlayback，不能再用下一包补满。
  if (impl_->ending || (impl_->tts.Size() != 0U &&
                        impl_->tts[impl_->tts.Size() - 1U].used != kTtsFrameSamples)) {
    return QueueTtsResult::Ending;
  }
  if (impl_->tts.Size() == kTtsSlots) {
    return QueueTtsResult::Full;
  }
  if (sequence != impl_->next_tts_sequence) {
    return QueueTtsResult::Discontinuous;
  }
  // 每次完整赋值一个零初始化槽，短尾帧不会带入旧音频。
  Impl::TtsSlot slot{};
  for (std::size_t i = 0U; i < samples; ++i) {
    // 一包对应一个槽。显式解码 S16_LE，不依赖 CPU 端序或字节地址对齐。
    const std::uint16_t value = static_cast<std::uint16_t>(bytes[2U * i]) |
                                (static_cast<std::uint16_t>(bytes[2U * i + 1U]) << 8U);
    slot.pcm[i] = static_cast<std::int16_t>(value);
  }
  slot.used = samples;
  static_cast<void>(impl_->tts.Push(slot));
  // 从0开始检查整条播放流，缺帧或重复都不能静默续播。
  impl_->next_tts_sequence = sequence + 1U;
  impl_->condition.notify_one();
  return QueueTtsResult::Queued;
}

bool AudioTasks::BeginPlayback() noexcept {
  if (impl_ == nullptr || !impl_->open) {
    return false;
  }
  std::lock_guard<std::mutex> lock(impl_->mutex);
  if (impl_->active) {
    return false;
  }
  impl_->ClearPlaybackQueue();
  // 先等 capture 在帧边界武装 AEC；置 active 后，播放线程才允许准备并渲染首帧。
  if (!impl_->RunCaptureCommand(Impl::CaptureCommand::ArmPlayback)) {
    return false;
  }
  impl_->ClearError();
  impl_->next_tts_sequence = 0U;
  impl_->ending = impl_->drop = impl_->playback_started = impl_->rebuffering = false;
  impl_->active = true;
  impl_->playback_scale.store(1.0F);
  impl_->playback_failed.store(false);
  impl_->is_done.store(false);
  return true;
}

bool AudioTasks::EndPlayback() noexcept {
  if (impl_ == nullptr || !impl_->open) {
    return false;
  }
  std::lock_guard<std::mutex> lock(impl_->mutex);
  if (!impl_->active || impl_->ending) {
    return false;
  }
  // ending 只声明生产结束；消费线程仍负责最后短帧和 ALSA drain。
  impl_->ending = true;
  impl_->condition.notify_one();
  return true;
}

void AudioTasks::SetPlaybackGain(const float gain) noexcept {
  if (impl_ == nullptr) {
    return;
  }
  impl_->playback_gain.store(std::max(0.0F, gain));
}

void AudioTasks::SetPlaybackScale(const float scale) noexcept {
  if (impl_ == nullptr) {
    return;
  }
  impl_->playback_scale.store(std::clamp(scale, 0.0F, 1.0F));
}

void AudioTasks::DropPlayback() noexcept {
  if (impl_ == nullptr || !impl_->open) {
    return;
  }
  std::lock_guard<std::mutex> lock(impl_->mutex);
  if (impl_->active) {
    impl_->drop = true;
    impl_->ClearPlaybackQueue();
    // 持有状态锁发出中断，保证播放线程不会在 drop 之前先完成并重置 PCM。
    impl_->pipeline.InterruptPlayback();
    impl_->condition.notify_one();
  }
}
bool AudioTasks::IsPlaybackDone() const noexcept {
  return impl_ == nullptr || impl_->is_done.load();
}

bool AudioTasks::HasPlaybackFailed() const noexcept {
  return impl_ != nullptr && impl_->playback_failed.load();
}

std::string AudioTasks::LastError() const {
  if (impl_ == nullptr) {
    return "audio is not allocated";
  }
  {
    std::lock_guard<std::mutex> lock(impl_->error_mutex);
    if (impl_->error[0] != '\0') {
      return impl_->error.data();
    }
  }
  return impl_->pipeline.LastError();
}

void AudioTasks::Stop() noexcept {
  if (impl_ == nullptr) {
    return;
  }
  if (impl_->open) {
    impl_->stop.store(true);
    // 与两个 condition_variable 建立同步边界，避免 stop notify 落在 waiter 入队前。
    { std::lock_guard<std::mutex> lock(impl_->capture_mutex); }
    impl_->capture_condition.notify_all();
    { std::lock_guard<std::mutex> lock(impl_->mutex); }
    impl_->condition.notify_all();
    impl_->pipeline.InterruptCapture();
    impl_->pipeline.InterruptPlayback();
    // abort/drop 让可能阻塞在 ALSA 的线程返回；join 后后端资源才可以安全释放。
    if (impl_->capture_thread.joinable()) {
      impl_->capture_thread.join();
    }
    if (impl_->playback_thread.joinable()) {
      impl_->playback_thread.join();
    }
  }
  impl_->pipeline.Close();
  impl_->raw_capture = {};
  impl_->open = impl_->active = impl_->ending = impl_->drop = impl_->playback_started =
      impl_->rebuffering = false;
  impl_->stop.store(false);
  impl_->capture_failed = false;
  impl_->capture_command = Impl::CaptureCommand::None;
  impl_->command_succeeded = false;
  impl_->next_tts_sequence = 0U;
  impl_->capture_sequence = 0U;
  impl_->capture.Clear();
  impl_->ClearPlaybackQueue();
  impl_->playback_failed.store(false);
  impl_->playback_scale.store(1.0F);
  impl_->is_done.store(true);
}

}  // namespace boompi::audio
