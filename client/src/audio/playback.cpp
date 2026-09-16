/** @file playback.cpp
 * @brief 播放任务：连续采样环 → 重采样/音量 → ALSA → 正常尾播完成通知。
 *
 * 主线程投递 PCM 和控制，播放线程消费；mutex 保护采样环与控制状态。
 * write/prepare/drain 由播放线程执行，cancel 可从主线程 drop 以打断阻塞输出。
 * 这里只管理音频消费，不决定新问题、追问窗口或网络轮次。
 */
#include "boompi/audio/playback.h"

#include <alsa/asoundlib.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <mutex>
#include <thread>

#include "audio_convert.h"
#include "audio_thread.h"
#include "board_voice_profile.h"
#include "boompi/debug.h"

namespace playback
{
static const std::size_t kCapacity = audio::kVoiceRateHz * 1500 / 1000;
// 采样环容量为1.5秒单声道音频，吸收网络到包速度与声卡消费速度的短时差异。
static std::array<std::int16_t, kCapacity> samples;
// head指向下一次播放的采样，buffered是有效采样数，两者都在mutex下更新。
static std::size_t head{0}, buffered{0};
static std::mutex mutex;
static std::condition_variable ready;
static std::thread thread;
static snd_pcm_t *device{nullptr};
static std::atomic<bool> stopping{false}, canceled{false};
// 音量可由主线程更新，播放线程每块读取一次后应用到整块输出。
static std::atomic<std::uint8_t> volume{60};
static State state{State::Idle};
// ending 表示输入已经结束；holding 表示插话试探请求，hold_applied 记录静音写入结果。
static bool ending{false}, holding{false};
static std::chrono::steady_clock::time_point hold_until{};
static std::atomic<bool> hold_applied{false};
static char failure[192]{};
static audio::StereoPlaybackFrame stereo;

/** @brief 持锁检查试探暂停是否尚未过期；截止时间在首次 true 请求时确定。 */
static bool hold_active()
{
    return holding && std::chrono::steady_clock::now() < hold_until;
}

// 启动时尚无工作线程；运行时所有错误写入均持mutex，保留最初失败原因。
static bool fail(const char *stage, int code = 0)
{
    if (!failure[0])
    {
        std::snprintf(failure, sizeof(failure), code ? "%s (%d)" : "%s", stage, code);
    }
    return false;
}

/** @brief 对已准备的 stereo 应用音量并完整写声卡，不决定本块来自正文、静音或尾音。
 * 只由播放线程在不持队列锁时调用；部分写从后缀继续，取消返回 -ECANCELED。
 */
static int write_device()
{
    // 先找整块峰值，以同一增益缩放这一块，保持块内采样的相对幅度。
    long peak = 0;
    for (std::size_t i = 0; i < stereo.frames * 2; ++i)
    {
        peak = std::max(peak, std::abs(static_cast<long>(stereo.pcm[i])));
    }
    // 百分比音量在输出端统一应用，整块峰值限制到95%满幅；用long计算绝对值覆盖S16范围。
    float gain = volume.load() / 100.0F;
    if (peak)
    {
        gain = std::min(gain, 31128.0F / static_cast<float>(peak));
    }
    for (std::size_t i = 0; i < stereo.frames * 2; ++i)
    {
        stereo.pcm[i] = static_cast<std::int16_t>(stereo.pcm[i] * gain);
    }
    std::size_t offset = 0;
    // ALSA返回每通道的采样时刻数；交错双声道的内存偏移需要乘2。
    while (offset < stereo.frames)
    {
        if (canceled.load() || stopping.load())
        {
            return -ECANCELED;
        }
        const int count = static_cast<int>(
            snd_pcm_writei(device, stereo.pcm.data() + 2 * offset, stereo.frames - offset));
        if (count > 0)
        {
            offset += static_cast<std::size_t>(count);
        }
        else if (canceled.load() && (count == -EBADFD || count == -EINTR || count == -EPIPE))
        {
            return -ECANCELED;
        }
        else if (count == -EPIPE || count == -ESTRPIPE)
        {
            // 欠载或挂起后重新准备PCM，保留offset指向的剩余内容继续交付。
            const int result = snd_pcm_prepare(device);
            if (result < 0)
            {
                return result;
            }
            // offset记录声卡已接受的采样时刻，恢复后从剩余后缀继续写。
            debug::log.playback_xrun_cb(offset, count);
        }
        else if (count != -EINTR)
        {
            return count < 0 ? count : -EIO;
        }
    }
    return static_cast<int>(stereo.frames);
}

/** @brief 正文已经交付完毕后，取出滤波尾音并等待声卡播完。
 * 播放线程在释放队列锁后执行；取消打断 write/drain，并由外层清理剩余数据。
 */
static int drain_output()
{
    // 转换器可能保留滤波延迟中的采样，持续取出直到有效输出长度归零。
    for (;;)
    {
        if (!audio_convert::playback(nullptr, 0, stereo))
        {
            return -EIO;
        }
        if (stereo.frames == 0)
        {
            break;
        }
        const int result = write_device();
        if (result < 0)
        {
            return result;
        }
    }
    // drain等待ALSA内核缓冲中的声音真正播放结束，完成后外层才发布Drained。
    const int result = snd_pcm_drain(device);
    if (canceled.load() && (result == -EBADFD || result == -EINTR))
    {
        return -ECANCELED;
    }
    return result;
}

/** @brief 丢弃声卡尚未播放的数据；调用方持 mutex，保留除已停止状态以外的错误。 */
static void drop()
{
    const int result = snd_pcm_drop(device);
    if (result < 0 && result != -EBADFD)
    {
        fail("playback drop failed", result);
    }
}

/** @brief 等待输入 → 每块取出后解锁写声卡 → 尾播或取消 → 通知主线程。
 * 写声卡期间释放队列锁，使主线程可继续投递或取消；每次写入前检查取消标志。
 */
static void play()
{
    audio::SetAudioThreadPriority("boompi-playback", 30);
    std::unique_lock<std::mutex> lock(mutex);
    while (!stopping.load())
    {
        // wait睡眠期间释放mutex；数据、取消或退出通知到来后再持锁检查条件。
        ready.wait(lock,
                   []
                   {
                       return stopping.load() || canceled.load() ||
                              (state == State::Playing && (ending || buffered != 0));
                   });
        if (stopping.load())
        {
            break;
        }
        // 1. 准备声卡和独立的滤波历史；新回答的第一块数据从初始转换状态开始。
        int result = canceled.load() ? -ECANCELED : snd_pcm_prepare(device);
        if (result >= 0 && !audio_convert::reset_playback())
        {
            result = -EIO;
        }
        while (result >= 0)
        {
            if (canceled.load() || stopping.load())
            {
                result = -ECANCELED;
                break;
            }
            // 2. 插话试探期间直接写静音，采样环和转换器历史都保持原位。
            if (hold_active())
            {
                stereo.pcm.fill(0);
                stereo.frames = audio::kDeviceFrameSamples;
                lock.unlock();
                result = write_device();
                lock.lock();
                // 写入成功后发布观察结果，采集线程将它与当前数字回采一起交给speech。
                hold_applied.store(result >= 0 && hold_active() && !canceled.load());
                continue;
            }
            hold_applied.store(false);
            // 3. 通常等完整 320 点；finish 后立即消费剩余短帧，队列为空才进入尾播。
            if (buffered < audio::kVoiceFrameSamples && !ending)
            {
                ready.wait(lock);
                continue;  // 唤醒后重新检查取消、暂停和数据量，不在等待条件里修改状态。
            }
            if (buffered == 0)
            {
                break;
            }
            audio::VoiceFrame16k frame;
            // 正文按一帧取出；输入结束时取出不足一帧的最后一段。
            const auto count = std::min(buffered, frame.size());
            for (std::size_t i = 0; i < count; ++i)
            {
                frame[i] = samples[(head + i) % kCapacity];
            }
            head = (head + count) % kCapacity;
            buffered -= count;
            // 当前块已经复制到线程局部变量，释放队列锁后主线程可继续追加采样。
            lock.unlock();
            if (!audio_convert::playback(frame.data(), count, stereo))
            {
                result = -EIO;
            }
            else
            {
                result = write_device();
            }
            lock.lock();
        }
        // 4. 只有正常结束才排尾音；解锁期间仍可取消，所以重新持锁后再决定最终状态。
        if (result >= 0 && !canceled.load() && !stopping.load())
        {
            lock.unlock();
            result = drain_output();
            lock.lock();
        }
        const bool discard = canceled.load() || stopping.load() || result == -ECANCELED;
        // 取消返回Idle，正常排空返回Drained；设备错误通过failure优先呈现为Failed。
        if (result < 0 && result != -ECANCELED)
        {
            fail("playback render/drain failed", result);
        }
        if (discard || failure[0])
        {
            drop();
        }
        state = discard ? State::Idle : State::Drained;
        buffered = 0;
        ending = holding = false;
        canceled.store(false);
        hold_applied.store(false);
        // 唤醒等待取消完成的write调用，使下一轮可接纳自己的首包音频。
        ready.notify_all();
    }
}

bool open(std::uint8_t level)
{
    failure[0] = '\0';
    int result = snd_pcm_open(
        &device, board_voice::kPlaybackPcm, SND_PCM_STREAM_PLAYBACK,
        SND_PCM_NO_AUTO_RESAMPLE | SND_PCM_NO_AUTO_CHANNELS | SND_PCM_NO_AUTO_FORMAT);
    if (result >= 0)
    {
        result =
            snd_pcm_set_params(device, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED,
                               audio::kPlaybackChannels, audio::kDeviceRateHz, 0, 80000);
    }
    if (result < 0)
    {
        close();
        return fail("playback open/configuration failed", result);
    }
    if (!audio_convert::open_playback())
    {
        close();
        return fail("playback resampler initialization failed");
    }
    set_volume(level);
    state = State::Idle;
    stopping.store(false);
    // std::thread 的失败通过标准异常返回；声卡与转换器仍按各自返回值判断。
    try
    {
        thread = std::thread(play);
        return true;
    }
    catch (const std::exception &)
    {
        close();
        return fail("playback thread creation failed");
    }
}

WriteResult write(const void *data, std::size_t bytes)
{
    // 网络音频为S16小端字节串，两个字节组成一个完整采样。
    const auto *pcm = static_cast<const std::uint8_t *>(data);
    if (!pcm || !bytes || bytes % 2)
    {
        return WriteResult::InvalidArgument;
    }
    std::unique_lock<std::mutex> lock(mutex);
    // 取消由播放线程收尾，此处只在取消进行中等待其离开Playing。
    if (canceled.load() && !ready.wait_for(lock, std::chrono::milliseconds(60),
                                           []
                                           {
                                               return state != State::Playing ||
                                                      stopping.load();
                                           }))
    {
        fail("playback cancel timed out");
        return WriteResult::Rejected;
    }
    if (!thread.joinable() || stopping.load() || failure[0])
    {
        return WriteResult::Rejected;
    }
    if (state == State::Playing && ending)
    {
        return WriteResult::Rejected;
    }
    const auto count = bytes / 2;
    // 容量检查覆盖整包采样；Full交给应用取消当前回答，保持语音内容的完整性。
    if (count > kCapacity - buffered)
    {
        return WriteResult::Full;
    }
    if (state != State::Playing)
    {
        state = State::Playing;
        ending = false;
    }
    // 网络负载在这里解码成连续采样，环形缓冲按采样顺序交给声卡消费。
    for (std::size_t i = 0; i < count; ++i)
    {
        // 低字节与高字节组合成S16，写入现有有效数据的末尾。
        samples[(head + buffered + i) % kCapacity] = static_cast<std::int16_t>(
            pcm[2 * i] | (static_cast<unsigned>(pcm[2 * i + 1]) << 8));
    }
    buffered += count;
    ready.notify_all();
    return WriteResult::Queued;
}

void finish()
{
    // 输入结束使播放线程放行不足320点的尾帧，并在取空环形缓冲后执行drain。
    std::lock_guard<std::mutex> lock(mutex);
    ending = true;
    ready.notify_all();
}

void cancel()
{
    // 主线程清软件队列并drop声卡缓冲，播放线程随后确认中断并归还Idle状态。
    std::lock_guard<std::mutex> lock(mutex);
    holding = false;
    hold_applied.store(false);
    if (state == State::Playing)
    {
        canceled.store(true);
        buffered = 0;
        drop();
        ready.notify_all();
    }
}

void hold(bool enabled)
{
    // 首次开启时设定绝对截止时刻；重复请求沿用同一窗口，限制试探暂停时长。
    std::lock_guard<std::mutex> lock(mutex);
    if (enabled && !holding)
    {
        // 预留700ms下行容量供试探期间接收；剩余容量决定本次暂停窗口能否开启。
        const bool room = buffered <= kCapacity - audio::kVoiceRateHz * 700 / 1000;
        hold_until =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(room ? 500 : 0);
    }
    holding = enabled && state == State::Playing && !canceled.load();
    if (!holding)
    {
        hold_applied.store(false);
    }
    ready.notify_all();
}

bool held()
{
    return hold_applied.load();
}

void set_volume(std::uint8_t level)
{
    volume.store(std::min<std::uint8_t>(level, 100));
}

State status()
{
    std::lock_guard<std::mutex> lock(mutex);
    return failure[0] ? State::Failed : state;
}

std::string error()
{
    std::lock_guard<std::mutex> lock(mutex);
    return failure;
}

void close()
{
    stopping.store(true);
    cancel();
    ready.notify_all();
    if (thread.joinable())
    {
        thread.join();
    }
    if (device)
    {
        snd_pcm_close(device);
        device = nullptr;
    }
    audio_convert::close_playback();
    std::lock_guard<std::mutex> lock(mutex);
    state = State::Idle;
    buffered = 0;
    ending = holding = false;
    canceled.store(false);
    hold_applied.store(false);
}
}  // namespace playback
