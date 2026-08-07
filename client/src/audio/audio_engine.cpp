/// @file 音频编排：有界 TTS 队列、唯一播放线程和跨线程时序。
#include "boompi/audio/audio_engine.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <new>
#include <pthread.h>
#include <sched.h>
#include <thread>

#include "audio_backend.h"

namespace boompi::audio { namespace {
// 服务端 TTS 为 24 kHz mono，因此 480 samples 与上行 320 samples 同样表示 20 ms。
constexpr std::size_t kTts24FrameSamples = 480U;
constexpr std::size_t kTtsSlots = 75U;       // 75 × 20 ms = 1.5 s，下行抖动上限。
constexpr std::size_t kInitialTtsSlots = 9U; // 首播累积 180 ms；EOS 短回复例外。
constexpr std::size_t kRebufferTtsSlots = 2U;// 确认真欠载后积累 40 ms 再恢复。
// 短于 30 ms 的到包间隔波动由 ALSA 缓冲吸收，避免频繁停播造成周期性卡顿。
constexpr auto kRebufferGrace = std::chrono::milliseconds(30);
constexpr std::size_t kCaptureSlots = 4U;    // actor 最多落后 80 ms；再慢就显式断帧。
// 控制命令必须快速跨越下一个 20 ms 帧边界；100 ms 允许少量调度抖动并限制关停等待。
constexpr auto kCaptureCommandTimeout = std::chrono::milliseconds(100);
// capture 优先级更高，因为丢失硬件输入不可恢复；playback 数据仍可由环形缓冲吸收抖动。
constexpr int kCapturePriority = 40;
constexpr int kPlaybackPriority = 30;

void SetRealtimePriority(const char* name, int priority) noexcept {
  // SCHED_FIFO 是板端低延迟优化。缺少权限时保留普通线程继续运行，日志用于确认部署能力。
#if defined(__APPLE__)
  static_cast<void>(pthread_setname_np(name));
#else
  static_cast<void>(pthread_setname_np(pthread_self(), name));
#endif
  sched_param parameters{};
  parameters.sched_priority = priority;
  const int result = pthread_setschedparam(
      pthread_self(), SCHED_FIFO, &parameters);
  if (result != 0) {
    std::fprintf(stderr,
                 "boompi-client: warning: %s realtime priority %d failed (%d)\n",
                 name, priority, result);
  }
}
}

struct AudioEngine::Impl final {
  using Clock = std::chrono::steady_clock;

  // 一个 slot 对应 20 ms。EOS 允许最后一个 slot 只使用前 `used` 个 sample。
  struct TtsSlot final { std::array<std::int16_t, kTts24FrameSamples> pcm{}; std::size_t used{0U}; };
  // 控制命令由 application 发出，在 capture period 边界由 capture_thread 执行。
  // 这条单槽握手保证 Snowboy/VAD/后端状态始终只有采集线程写入。
  enum class CaptureCommand : std::uint8_t {
    kNone, kResetListener, kPreparePlayback
  };

  // capture_thread 独占采集/3A，并在采集帧边界执行后端控制命令；
  // BeginPlayback 完成该握手后，playback_thread 独占播放热路径。
  platform::rv1106::AudioBackend backend{};
  std::thread capture_thread{}, playback_thread{};
  // mutex 保护 TTS/播放状态；capture_mutex 保护采集队列和命令；error_mutex 仅保护错误文本。
  mutable std::mutex mutex{}, capture_mutex{}, error_mutex{};
  std::condition_variable condition{}, capture_condition{};
  std::array<char, 192U> error{};
  // capture 是单生产者/单消费者有界环。满时清环并发布一个显式 discontinuity 帧。
  std::array<CaptureFrame, kCaptureSlots> capture{};
  std::size_t capture_head{0U}, capture_count{0U};
  CaptureCommand capture_command{CaptureCommand::kNone};
  bool command_succeeded{false};
  std::array<TtsSlot, kTtsSlots> tts{};
  std::size_t tts_head{0U}, tts_tail{0U}, tts_count{0U}, queued_samples{0U};
  // capture_sequence 描述本地处理时间线；next_tts_sequence 检查服务端下行包是否连续。
  std::uint64_t capture_sequence{0U}, next_tts_sequence{0U}, rebuffer_events{0U};
  // capture 指标覆盖整个 Open 生命周期：XRUN 与 actor 落后分开计数，水位用 20 ms 帧表示。
  std::uint64_t capture_xruns{0U}, capture_actor_overruns{0U};
  std::size_t capture_high_water_frames{0U};
  // 以下指标按一次播放会话重置，用于把网络到包抖动与 ALSA 播放问题分开定位。
  std::size_t tts_high_water_samples{0U};
  std::uint64_t maximum_pcm_gap_ms{0U};
  std::uint64_t rebuffer_total_ms{0U}, rebuffer_maximum_ms{0U};
  std::uint64_t playback_xrun_baseline{0U};
  Clock::time_point last_pcm_at{}, rebuffer_started_at{};
  bool open{false}, capture_failed{false};
  // `active` 覆盖 Begin 到 drain/drop；`ending` 表示网络已声明没有后续 PCM。
  bool active{false}, ending{false}, playback_started{false}, rebuffering{false};
  bool drop{false}, sequence_set{false};
  // stop 跨两条线程发布生命周期；gain/scale 和完成状态允许 UI/application 无锁读取。
  std::atomic<bool> stop{false};
  std::atomic<float> playback_gain{1.0F}, playback_scale{1.0F};
  std::atomic<bool> is_done{true}, playback_failed{false};

  void SetError(const char* text) noexcept {
    std::lock_guard<std::mutex> lock(error_mutex);
    std::snprintf(error.data(), error.size(), "%s", text);
  }
  void ClearError() noexcept {
    std::lock_guard<std::mutex> lock(error_mutex);
    error.fill('\0');
  }
  void ClearQueue() noexcept {
    // 清零同时移除旧语音，防止槽复用时 EOS 短帧尾部带入上一轮数据。
    for (auto& slot : tts) { slot.pcm.fill(0); slot.used = 0U; }
    tts_head = tts_tail = tts_count = queued_samples = 0U;
  }

  void ResetPlaybackMetrics() noexcept {
    tts_high_water_samples = 0U;
    maximum_pcm_gap_ms = 0U;
    rebuffer_events = rebuffer_total_ms = rebuffer_maximum_ms = 0U;
    last_pcm_at = rebuffer_started_at = {};
  }

  void ResetCaptureMetrics() noexcept {
    capture_xruns = capture_actor_overruns = 0U;
    capture_high_water_frames = 0U;
  }

  void LogAudioSummary() const noexcept {
    std::fprintf(
        stderr,
        "boompi-client: audio summary; capture_xruns=%llu; playback_xruns=%llu; "
        "capture_queue_high_water_frames=%zu; actor_queue_overruns=%llu\n",
        static_cast<unsigned long long>(capture_xruns),
        static_cast<unsigned long long>(backend.playback_xruns()),
        capture_high_water_frames,
        static_cast<unsigned long long>(capture_actor_overruns));
  }

  void FinishRebufferInterval(const Clock::time_point now) noexcept {
    // 只在进入重缓冲时记录起点；正常播放路径调用本函数不会制造零时长事件。
    if (rebuffer_started_at == Clock::time_point{}) return;
    const auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - rebuffer_started_at);
    const auto milliseconds = static_cast<std::uint64_t>(duration.count());
    rebuffer_total_ms += milliseconds;
    rebuffer_maximum_ms = std::max(rebuffer_maximum_ms, milliseconds);
    rebuffer_started_at = {};
  }

  void ClearCaptureQueue() noexcept {
    for (auto& frame : capture) frame = {};
    capture_head = capture_count = 0U;
  }

  void ClearQueuedListenerDecisions() noexcept {
    // PCM 和连续性元数据保留在原时间轴，只清除旧监听阶段产生的业务判断。
    for (std::size_t offset = 0U; offset < capture_count; ++offset) {
      auto& frame = capture[(capture_head + offset) % capture.size()];
      frame.wake = frame.vad_now = frame.vad_started = frame.vad_ended = false;
      frame.near_voice = false;
    }
  }

  bool RunCaptureCommand(CaptureCommand command) noexcept {
    // application 在这里同步等待 capture_thread 确认，后端方法仍由唯一 owner 调用。
    std::unique_lock<std::mutex> lock(capture_mutex);
    if (stop.load(std::memory_order_acquire) || capture_failed ||
        capture_command != CaptureCommand::kNone) return false;
    capture_command = command;
    capture_condition.notify_all();
    if (!capture_condition.wait_for(lock, kCaptureCommandTimeout,
                                    [this] {
      return stop.load(std::memory_order_acquire) || capture_failed ||
             capture_command == CaptureCommand::kNone;
    })) {
      // ALSA 没有交出下一个 period 时，命令无法安全越过帧边界执行。明确失败
      // 可让 application 退出并由 Close 中断 readi，不能把停止请求永久卡在这里。
      capture_failed = true;
      SetError("capture control timed out");
      capture_condition.notify_all();
      return false;
    }
    return !stop.load(std::memory_order_acquire) && !capture_failed &&
           capture_command == CaptureCommand::kNone && command_succeeded;
  }

  bool ApplyCaptureCommand() noexcept {
    // 后端调用不能持有 capture_mutex，否则 application 无法观察停止和命令完成。
    CaptureCommand command = CaptureCommand::kNone;
    {
      std::lock_guard<std::mutex> lock(capture_mutex);
      if (capture_failed) return false;
      if (capture_command == CaptureCommand::kNone) return true;
      command = capture_command;
    }
    const bool succeeded = command == CaptureCommand::kResetListener
                               ? backend.ResetListener()
                               : command == CaptureCommand::kPreparePlayback
                                     ? backend.PreparePlayback()
                                     : false;
    {
      std::lock_guard<std::mutex> lock(capture_mutex);
      // 已采集 PCM 仍属于时间轴，不能因 listener reset 被静默丢弃；但 reset 前
      // 的 Snowboy/VAD 判定不得穿越状态边界。reference、dBFS 和连续性标记保持原样。
      if (command == CaptureCommand::kResetListener)
        ClearQueuedListenerDecisions();
      command_succeeded = succeeded;
      capture_command = CaptureCommand::kNone;
    }
    capture_condition.notify_all();
    return succeeded;
  }

  void CaptureLoop() noexcept {
    SetRealtimePriority("boompi-capture", kCapturePriority);
    for (;;) {
      // 每轮先处理控制命令，再阻塞读取一个 period，形成稳定的 20 ms 状态边界。
      if (!ApplyCaptureCommand()) {
        std::lock_guard<std::mutex> lock(capture_mutex);
        capture_failed = true;
        capture_condition.notify_all();
        break;
      }
      if (stop.load(std::memory_order_acquire)) break;
      CaptureFrame frame{};
      bool discontinuity = false;
      bool succeeded = backend.ReadCapture20ms(&discontinuity);
      if (succeeded && !stop.load(std::memory_order_acquire))
        succeeded = backend.ProcessCapture20ms(discontinuity, &frame);
      if (succeeded && !stop.load(std::memory_order_acquire)) {
        frame.sequence = capture_sequence++;
        std::lock_guard<std::mutex> queue_lock(capture_mutex);
        // XRUN 与 actor 落后都破坏连续音频。清空旧帧后交付带原因的新边界，
        // application 会终止当前 turn，避免把时间洞伪装成连续语音上传。
        const bool capture_xrun = frame.discontinuity;
        const bool actor_overrun = capture_count == capture.size();
        if (capture_xrun) ++capture_xruns;
        if (actor_overrun) ++capture_actor_overruns;
        if (capture_xrun || actor_overrun) {
          ClearCaptureQueue();
          frame.discontinuity = true;
          // 同一帧也可能同时观察到 XRUN 和满队列；上传边界优先报告硬件断点，
          // 聚合计数仍分别保留两个事实。
          frame.actor_overrun = actor_overrun && !capture_xrun;
        }
        const std::size_t tail =
            (capture_head + capture_count) % capture.size();
        capture[tail] = frame;
        ++capture_count;
        capture_high_water_frames =
            std::max(capture_high_water_frames, capture_count);
      }
      if (stop.load(std::memory_order_acquire)) break;
      if (!succeeded) {
        std::lock_guard<std::mutex> lock(capture_mutex);
        capture_failed = true;
        capture_condition.notify_all();
        break;
      }
      capture_condition.notify_one();
    }
  }

  /// 播放线程只负责下行渲染；AEC reference 由 Codec Mode1 与双麦在同一 capture period 回采。
  bool Render(const TtsSlot& slot) noexcept {
    const float gain = playback_gain.load(std::memory_order_relaxed) *
                       playback_scale.load(std::memory_order_relaxed);
    return backend.Render20ms(slot.pcm.data(), slot.used, gain);
  }

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
    if (discard || failed)
      backend.DropPlayback();
    else
      failed = !backend.DrainPlayback();
    std::lock_guard<std::mutex> lock(mutex);
    // DropPlayback 持有同一把锁先中断 PCM；因此这里可安全完成 prepare。
    if (drop && !discard) { discard = true; failed = false; backend.DropPlayback(); }
    FinishRebufferInterval(Clock::now());
    const std::uint64_t playback_xruns =
        backend.playback_xruns() - playback_xrun_baseline;
    std::fprintf(
        stderr,
        "boompi-client: TTS playback summary; playback_queue_high_water_ms=%llu; "
        "playback_xruns=%llu; max_pcm_gap_ms=%llu; rebuffer_events=%llu; rebuffer_total_ms=%llu; "
        "rebuffer_max_ms=%llu\n",
        static_cast<unsigned long long>(tts_high_water_samples * 1000U /
                                        24000U),
        static_cast<unsigned long long>(playback_xruns),
        static_cast<unsigned long long>(maximum_pcm_gap_ms),
        static_cast<unsigned long long>(rebuffer_events),
        static_cast<unsigned long long>(rebuffer_total_ms),
        static_cast<unsigned long long>(rebuffer_maximum_ms));
    active = ending = drop = playback_started = rebuffering = false;
    ResetPlaybackMetrics();
    ClearQueue();
    playback_scale.store(1.0F, std::memory_order_release);
    playback_failed.store(failed, std::memory_order_release);
    is_done.store(true, std::memory_order_release);
    condition.notify_all();
  }

  /// 此线程只消费 TTS 并写 ALSA；绝不访问 capture、会话状态或网络。
  void PlaybackLoop() noexcept {
    SetRealtimePriority("boompi-playback", kPlaybackPriority);
    for (;;) {
      TtsSlot slot{}; bool have_slot = false, finish = false, must_drop = false;
      {
        std::unique_lock<std::mutex> lock(mutex);
        // 单次调度抖动不进入重缓冲；只有连续 30 ms 没有新 PCM 才重新蓄水。
        if (active && playback_started && tts_count == 0U && !ending &&
            !drop && !rebuffering &&
            !condition.wait_for(lock, kRebufferGrace, [this] {
              return stop.load(std::memory_order_acquire) || drop || ending ||
                     tts_count != 0U;
            })) {
          rebuffering = true;
          rebuffer_started_at = Clock::now();
          ++rebuffer_events;
        }
        condition.wait(lock, [this] {
          // 首播需要 180 ms 抵抗网络首包抖动；发生真实欠载后只蓄 40 ms，缩短恢复停顿。
          // ending 放行短回复和最后一个不足 20 ms 的 slot，保证尾音可以完成。
          const bool initial_ready = queued_samples >= kInitialTtsSlots * kTts24FrameSamples;
          const bool resume_ready = queued_samples >= kRebufferTtsSlots * kTts24FrameSamples;
          const bool buffer_ready = playback_started
              ? (!rebuffering || resume_ready || ending)
              : (initial_ready || ending);
          return stop.load(std::memory_order_acquire) || drop ||
              (active && ((tts_count != 0U &&
                (tts[tts_head].used == kTts24FrameSamples || ending) && buffer_ready) ||
                (ending && tts_count == 0U)));
        });
        if (stop.load(std::memory_order_acquire)) break;
        must_drop = drop;
        if (!must_drop && tts_count != 0U) {
          // 在锁内取走一个 slot，随后释放锁做重采样和 ALSA write，网络线程可继续入队。
          FinishRebufferInterval(Clock::now());
          rebuffering = false;
          playback_started = true;
          slot = tts[tts_head];
          queued_samples -= slot.used;
          tts[tts_head] = {};
          tts_head = (tts_head + 1U) % tts.size();
          --tts_count;
          have_slot = true;
        }
        finish = must_drop || (ending && tts_count == 0U);
      }
      bool failed = have_slot && !Render(slot);
      if (finish || failed) FinishPlayback(must_drop, failed);
    }
  }

};

AudioEngine::~AudioEngine() noexcept { Close(); delete impl_; }

bool AudioEngine::Open(const AudioEngineConfig& config) noexcept {
  // 后端先完整就绪，再发布 open 并创建线程，线程启动后不会看到半初始化资源。
  if (impl_ == nullptr) impl_ = new (std::nothrow) Impl;
  if (impl_ == nullptr) return false;
  if (impl_->open) { impl_->SetError("audio is already open"); return false; }
  if (config.playback_gain < 0.0F) {
    impl_->SetError("invalid playback gain configuration"); return false;
  }
  if (!impl_->backend.Open(config)) return false;
  impl_->playback_gain.store(config.playback_gain, std::memory_order_release);
  impl_->stop.store(false, std::memory_order_release);
  impl_->capture_failed = false;
  impl_->capture_command = Impl::CaptureCommand::kNone;
  impl_->command_succeeded = false;
  impl_->capture_sequence = 0U;
  impl_->ClearCaptureQueue();
  impl_->ResetCaptureMetrics();
  impl_->open = true;
  try {
    impl_->capture_thread = std::thread(&Impl::CaptureLoop, impl_);
    impl_->playback_thread = std::thread(&Impl::PlaybackLoop, impl_);
  } catch (...) {
    impl_->SetError("audio realtime thread creation failed");
    Close();
    return false;
  }
  impl_->ClearError();
  return true;
}

CaptureResult AudioEngine::Capture(
    CaptureFrame* const frame,
    const std::chrono::milliseconds timeout) noexcept {
  // 高优先级采集线程持续排空 ALSA；application actor 只消费已处理的固定帧。
  if (impl_ == nullptr || !impl_->open || frame == nullptr ||
      timeout < std::chrono::milliseconds::zero())
    return CaptureResult::kFailed;
  std::unique_lock<std::mutex> lock(impl_->capture_mutex);
  if (!impl_->capture_condition.wait_for(lock, timeout, [this] {
    return impl_->stop.load(std::memory_order_acquire) ||
           impl_->capture_failed || impl_->capture_count != 0U;
  }))
    return CaptureResult::kTimeout;
  if (impl_->stop.load(std::memory_order_acquire) ||
      impl_->capture_failed) return CaptureResult::kFailed;
  *frame = impl_->capture[impl_->capture_head];
  impl_->capture[impl_->capture_head] = {};
  impl_->capture_head = (impl_->capture_head + 1U) % impl_->capture.size();
  --impl_->capture_count;
  return CaptureResult::kFrame;
}

bool AudioEngine::ResetListener() noexcept {
  if (impl_ == nullptr || !impl_->open ||
      impl_->stop.load(std::memory_order_acquire)) return false;
  return impl_->RunCaptureCommand(Impl::CaptureCommand::kResetListener);
}

QueueTtsResult AudioEngine::QueueTts24k(
    const std::uint8_t* const bytes, const std::size_t byte_count,
    const std::uint64_t sequence) noexcept {
  if (impl_ == nullptr || !impl_->open) return QueueTtsResult::kNotOpen;
  if (bytes == nullptr || byte_count == 0U || (byte_count & 1U) != 0U)
    return QueueTtsResult::kInvalidArgument;
  const std::size_t samples = byte_count / 2U;
  // WSS 事件已经回到 actor 线程，此处是 TTS ring 的唯一生产者。
  std::lock_guard<std::mutex> lock(impl_->mutex);
  if (!impl_->active) return QueueTtsResult::kNotActive;
  if (impl_->ending) return QueueTtsResult::kEnding;
  if (samples > kTtsSlots * kTts24FrameSamples - impl_->queued_samples)
    return QueueTtsResult::kFull;
  if (impl_->sequence_set && sequence != impl_->next_tts_sequence)
    return QueueTtsResult::kDiscontinuous;
  const auto queued_at = Impl::Clock::now();
  std::size_t source = 0U;
  while (source < samples) {
    // 网络包大小与 20 ms slot 无关：可填满前一槽，也可跨多个槽，媒体序号按包检查。
    if (impl_->tts_count == 0U || impl_->tts[(impl_->tts_tail + kTtsSlots - 1U) % kTtsSlots].used == kTts24FrameSamples) {
      impl_->tts_tail = (impl_->tts_tail + 1U) % kTtsSlots;
      ++impl_->tts_count;
    }
    auto& slot = impl_->tts[(impl_->tts_tail + kTtsSlots - 1U) % kTtsSlots];
    const std::size_t copied = std::min(samples - source, kTts24FrameSamples - slot.used);
    for (std::size_t i = 0U; i < copied; ++i) {
      // 线协议固定为 little-endian S16，显式解码避免依赖 CPU 端序和未对齐访问。
      const std::size_t offset = 2U * (source + i);
      const std::uint16_t value = static_cast<std::uint16_t>(bytes[offset]) |
          (static_cast<std::uint16_t>(bytes[offset + 1U]) << 8U);
      slot.pcm[slot.used + i] = static_cast<std::int16_t>(value);
    }
    slot.used += copied;
    source += copied;
  }
  impl_->queued_samples += samples;
  impl_->tts_high_water_samples =
      std::max(impl_->tts_high_water_samples, impl_->queued_samples);
  if (impl_->last_pcm_at != Impl::Clock::time_point{}) {
    const auto gap = std::chrono::duration_cast<std::chrono::milliseconds>(
        queued_at - impl_->last_pcm_at);
    impl_->maximum_pcm_gap_ms = std::max(
        impl_->maximum_pcm_gap_ms,
        static_cast<std::uint64_t>(gap.count()));
  }
  impl_->last_pcm_at = queued_at;
  impl_->sequence_set = true;
  impl_->next_tts_sequence = sequence + 1U;
  impl_->condition.notify_one();
  return QueueTtsResult::kQueued;
}

bool AudioEngine::BeginPlayback() noexcept {
  if (impl_ == nullptr || !impl_->open) return false;
  std::lock_guard<std::mutex> lock(impl_->mutex);
  if (impl_->active) return false;
  impl_->ClearQueue();
  // application 在此等待 capture 线程于帧边界执行 prepare；
  // 返回后才置 active，随后的渲染热路径只属于 playback 线程。
  if (!impl_->RunCaptureCommand(Impl::CaptureCommand::kPreparePlayback))
    return false;
  impl_->ClearError();
  impl_->sequence_set = impl_->ending = impl_->drop = impl_->playback_started =
      impl_->rebuffering = false;
  impl_->ResetPlaybackMetrics();
  impl_->playback_xrun_baseline = impl_->backend.playback_xruns();
  impl_->active = true;
  impl_->playback_scale.store(1.0F, std::memory_order_release);
  impl_->playback_failed.store(false, std::memory_order_release);
  impl_->is_done.store(false, std::memory_order_release);
  return true;
}

bool AudioEngine::EndPlayback() noexcept {
  if (impl_ == nullptr || !impl_->open) return false;
  std::lock_guard<std::mutex> lock(impl_->mutex);
  if (!impl_->active || impl_->ending) return false;
  // ending 只声明生产结束；消费线程仍负责最后短帧和 ALSA drain。
  impl_->ending = true;
  impl_->condition.notify_one();
  return true;
}

void AudioEngine::SetPlaybackGain(const float gain) noexcept {
  if (impl_ != nullptr) impl_->playback_gain.store(std::max(0.0F, gain), std::memory_order_release);
}
void AudioEngine::SetPlaybackScale(const float scale) noexcept {
  if (impl_ != nullptr) impl_->playback_scale.store(std::clamp(scale, 0.0F, 1.0F), std::memory_order_release);
}
void AudioEngine::DropPlayback() noexcept {
  if (impl_ == nullptr || !impl_->open) return;
  std::lock_guard<std::mutex> lock(impl_->mutex);
  if (impl_->active) {
    impl_->drop = true;
    impl_->ClearQueue();
    // 持有状态锁发出中断，保证播放线程不会在 drop 之前先完成并重置 PCM。
    impl_->backend.InterruptPlayback();
    impl_->condition.notify_one();
  }
}
bool AudioEngine::playback_done() const noexcept { return impl_ == nullptr || impl_->is_done.load(std::memory_order_acquire); }
bool AudioEngine::playback_failed() const noexcept { return impl_ != nullptr && impl_->playback_failed.load(std::memory_order_acquire); }

std::string AudioEngine::last_error() const {
  if (impl_ == nullptr) return "audio is not allocated";
  {
    std::lock_guard<std::mutex> lock(impl_->error_mutex);
    if (impl_->error[0] != '\0') return impl_->error.data();
  }
  return impl_->backend.last_error();
}

void AudioEngine::Close() noexcept {
  if (impl_ == nullptr) return;
  if (impl_->open) {
    impl_->stop.store(true, std::memory_order_release);
    // 与两个 condition_variable 建立同步边界，避免 stop notify 落在 waiter 入队前。
    { std::lock_guard<std::mutex> lock(impl_->capture_mutex); }
    impl_->capture_condition.notify_all();
    { std::lock_guard<std::mutex> lock(impl_->mutex); }
    impl_->condition.notify_all();
    impl_->backend.InterruptCapture();
    impl_->backend.InterruptPlayback();
    // abort/drop 让可能阻塞在 ALSA 的线程返回；join 后后端资源才可以安全释放。
    if (impl_->capture_thread.joinable()) impl_->capture_thread.join();
    if (impl_->playback_thread.joinable()) impl_->playback_thread.join();
    impl_->LogAudioSummary();
  }
  impl_->backend.Close();
  impl_->open = impl_->active = impl_->ending = impl_->drop =
      impl_->playback_started = impl_->rebuffering = false;
  impl_->ResetPlaybackMetrics();
  impl_->playback_xrun_baseline = 0U;
  impl_->stop.store(false, std::memory_order_release);
  impl_->capture_failed = false;
  impl_->capture_command = Impl::CaptureCommand::kNone;
  impl_->command_succeeded = false;
  impl_->sequence_set = false;
  impl_->capture_sequence = 0U;
  impl_->ResetCaptureMetrics();
  impl_->ClearCaptureQueue();
  impl_->ClearQueue();
  impl_->playback_failed.store(false, std::memory_order_release);
  impl_->playback_scale.store(1.0F, std::memory_order_release);
  impl_->is_done.store(true, std::memory_order_release);
}

}  // namespace boompi::audio
