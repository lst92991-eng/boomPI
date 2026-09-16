/** @file voice_input.cpp
 * @brief 实时输入任务：ALSA → 格式适配 → Rockchip 3A → Snowboy → WebRTC VAD → 主线程。
 *
 * 算法在同一任务里顺序调用，只有最后交付处使用 4 帧（80ms）有界队列。
 * raw/channels 由采集任务独占；frames、读位置、错误原因在 mutex 下交接。
 */
#include "boompi/audio/voice_input.h"

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <exception>
#include <mutex>
#include <thread>

#include "audio_capture.h"
#include "audio_convert.h"
#include "audio_thread.h"
#include "board_voice_profile.h"
#include "boompi/audio/playback.h"
#include "boompi/debug.h"
#include "boompi/platform/rv1106/rockchip_3a.h"
#include "vad.h"
#include "wake.h"

namespace voice_input
{
static const std::size_t kCaptureSlots = 80 / audio::kFrameMs;
static std::mutex mutex;
// condition负责唤醒等待输入的主线程；mutex同时保护采样队列和失败原因。
static std::condition_variable condition;
static std::thread thread;
static std::atomic<bool> wake_reset{false};
static std::array<char, 192U> failure{};
static std::array<audio::CaptureFrame, kCaptureSlots> frames;
static std::size_t read_at{0}, pending{0};
// read_at指向最早的未读帧，pending记录数量；写入位置由两者相加取模得到。
// 模块预先分配设备帧和算法帧，采集循环直接复用这两块工作内存。
static audio::RawCaptureFrame raw;
static audio::CaptureChannels channels;

/** @brief 保存输入故障并唤醒主线程；应用读取 Failed 后结束运行。 */
static bool fail(const char *reason, int code = 0)
{
    std::lock_guard<std::mutex> lock(mutex);
    std::snprintf(failure.data(), failure.size(), code ? "%s (ALSA %d)" : "%s", reason, code);
    condition.notify_all();
    return false;
}

/** @brief 采集线程入口；每轮只读一块、处理一块、交付一块，不等待网络。 */
static void capture_task()
{
    audio::SetAudioThreadPriority("boompi-capture", 40);
    bool previous_reference = false, previous_held = false;
    // 仅用于日志边沿检测：-1表示尚未观察到有效结果，0/1对应静音和人声。
    int previous_vad = -1;
    for (;;)
    {
        // 1. 读取原始四槽 PCM；0 是断流，负值区分主动停止和设备故障。
        const bool held_before_read = playback::held();
        const int captured = audio_capture::read(raw.data());
        if (captured < 0)
        {
            if (captured != -ECANCELED)
            {
                fail("ALSA capture read failed", captured);
            }
            break;
        }
        audio::CaptureFrame frame{};
        if (captured == 0)
        {
            previous_vad = -1;
            // 硬件断流后清除滤波、3A和检测器历史，让下一帧从新的连续区间开始。
            previous_reference = previous_held = false;
            rockchip_3a::close();
            if (!audio_convert::reset_capture() || !rockchip_3a::open() || !wake::reset() ||
                !vad::reset())
            {
                fail("audio processing reset after discontinuity failed");
                break;
            }
            frame.discontinuity = true;
        }
        else
        {
            // Snowboy要求外部VAD句尾后Reset；只由本线程调用，不等待业务线程握手。
            // exchange一次取出并清除复位请求，检测器始终由采集线程串行访问。
            if (wake_reset.exchange(false) && !wake::reset())
            {
                fail("Snowboy reset failed");
                break;
            }
            // 2. 在采集线程顺序处理同一帧：格式转换 → 3A → 唤醒 → VAD。
            if (!audio_convert::capture(raw, channels))
            {
                fail("capture resampler lost frame alignment");
                break;
            }
            if (!rockchip_3a::process(channels, frame.pcm))
            {
                fail("Rockchip 3A rejected a frame");
                break;
            }
            const int detected = wake::detect(frame.pcm);
            if (detected < 0)
            {
                fail("Snowboy processing failed");
                break;
            }
            frame.wake = detected > 0;
            if (frame.wake)
            {
                debug::log.wake_detected_cb();
            }
            // 唤醒结果和VAD结果随同一块PCM交付，应用可按同一时间位置做决策。
            const int voice = vad::process(frame.pcm);
            if (voice < 0)
            {
                fail("WebRTC VAD processing failed");
                break;
            }
            frame.vad_now = voice == 1;
            // 首次有效结果及后续变化各打印一次，错误结果由前面的失败分支处理。
            if (voice != previous_vad)
            {
                debug::log.vad_changed_cb(frame.vad_now);
                previous_vad = voice;
            }

            // 3. 给处理结果附带插话所需的观测；参考和播放观测随 3A 预填延后一帧。
            frame.reference_active = previous_reference;
            frame.playback_held = previous_held;
            previous_held = held_before_read;
            previous_reference = false;
            for (std::size_t i = 2; i < channels.size(); i += 3)
            {
                // 三通道交错数据的第3项是refL，逐个采样时刻检查播放参考幅度。
                previous_reference = previous_reference ||
                                     channels[i] > board_voice::kReferencePeak ||
                                     channels[i] < -board_voice::kReferencePeak;
            }
        }
        // 4. 一次交付 PCM 和检测结果。队列满则发布断点，让应用取消残缺语句。
        std::lock_guard<std::mutex> lock(mutex);
        if (frame.discontinuity || pending == kCaptureSlots)
        {
            // 丢弃已失去连续性的排队数据，交付带断点标志的新帧通知应用结束当前语句。
            frame.discontinuity = true;
            read_at = pending = 0;
        }
        // PCM和检测标志作为一帧同时写入；notify在数据就绪后唤醒读取方。
        frames[(read_at + pending) % kCaptureSlots] = frame;
        ++pending;
        condition.notify_all();
    }
}

bool open()
{
    failure.fill('\0');
    const int result = audio_capture::open();
    if (result < 0)
    {
        return fail("ALSA capture initialization failed", result);
    }
    if (!audio_convert::open_capture())
    {
        close();
        return fail("capture resampler initialization failed");
    }
    if (!rockchip_3a::open())
    {
        close();
        return fail("Rockchip 3A initialization failed");
    }
    if (!wake::open())
    {
        close();
        return fail("Snowboy initialization failed");
    }
    if (!vad::open())
    {
        close();
        return fail("WebRTC VAD initialization failed");
    }
    wake_reset.store(false);
    read_at = pending = 0;
    return true;
}

bool start()
{
    // Mode1回采同时使用输入和输出链路，应用配置好两路PCM后再启动本线程。
    // std::thread 创建会抛标准异常；失败在这里转为 false，并回收已经打开的输入资源。
    try
    {
        thread = std::thread(capture_task);
    }
    catch (const std::exception &)
    {
        close();
        return fail("capture thread creation failed");
    }
    return true;
}

ReadResult read(audio::CaptureFrame &frame)
{
    // wait_for等待期间释放锁，采集线程可继续写入；唤醒后重新持锁检查条件。
    std::unique_lock<std::mutex> lock(mutex);
    if (!condition.wait_for(lock, std::chrono::milliseconds(20),
                            []
                            {
                                return failure[0] || pending != 0;
                            }))
    {
        return ReadResult::Timeout;
    }
    if (failure[0])
    {
        return ReadResult::Failed;
    }
    // 在锁内取出最早的处理帧并推进读位置，应用持有独立副本直到本轮处理结束。
    frame = frames[read_at];
    read_at = (read_at + 1) % kCaptureSlots;
    --pending;
    return ReadResult::Frame;
}

void end_utterance()
{
    // 主线程只发布请求，采集线程在下一轮读取后执行Snowboy复位。
    wake_reset.store(true);
}

std::string error()
{
    // 在锁内取得错误文本的副本，调用方可在释放锁后继续显示或记录。
    std::lock_guard<std::mutex> lock(mutex);
    return failure.data();
}

void close()
{
    // abort唤醒阻塞中的readi；join确认线程退出后，设备和算法句柄才可释放。
    const int interrupted = audio_capture::interrupt();
    if (interrupted < 0)
    {
        fail("ALSA capture stop failed", interrupted);
    }
    if (thread.joinable())
    {
        thread.join();
    }
    audio_capture::close();
    audio_convert::close_capture();
    rockchip_3a::close();
    wake::close();
    vad::close();
    std::lock_guard<std::mutex> lock(mutex);
    read_at = pending = 0;
}
}  // namespace voice_input
