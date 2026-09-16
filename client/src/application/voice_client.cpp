/** @file voice_client.cpp
 * @brief 小智应用主流程：初始化 → 处理回复/输入/触摸 → 停止并回收。
 *
 * 本文件由主线程执行，独占对话四态、字幕和等待期限。
 * 输入任务只交付 PCM/检测结果，网络拥有轮次与序号，播放任务报告实际尾播完成。
 */
#include "boompi/application/voice_client.h"

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <exception>

#include "boompi/audio/playback.h"
#include "boompi/audio/speech.h"
#include "boompi/audio/voice_input.h"
#include "boompi/config/voice_client_config.h"
#include "boompi/debug.h"
#include "boompi/network/voice_net.h"
#include "boompi/ui/device_ui.h"

// 单调时钟用于阶段超时，系统时间调整后计时仍连续。
typedef std::chrono::steady_clock Clock;

// 唤醒后留给用户开口的时间。
static const std::chrono::seconds kWakeWindow{6};
// 回答结束后继续接收追问的窗口。
static const std::chrono::seconds kFollowupWindow{3};
// 一句话占用上传阶段的最长时间。
static const std::chrono::seconds kUploadLimit{60};
// 回复到包后更新此期限，检测长时间停顿。
static const std::chrono::seconds kReplyIdleTimeout{30};
// 从END起计算的整轮回复总时限。
static const std::chrono::seconds kReplyTotalLimit{300};
// 收到DONE后留给本地播放器排空尾音。
static const std::chrono::seconds kDrainTimeout{3};

// 主线程根据唤醒、语句和播放事件切换对话阶段。
enum class AppState
{
    Idle,          // 等待唤醒。
    Listening,     // 等待开口或上传当前语句。
    WaitingReply,  // 等待服务端回答。
    Speaking       // 播放回答，接收插话。
};
static AppState state{AppState::Idle};
// 主线程维护这份画面数据，ui::show复制后交给UI线程绘制。
static ui::UiView view;

// 当前阶段的截止时刻与整轮回复的最晚结束时刻。
static Clock::time_point deadline{};
static Clock::time_point response_limit{};
// 固定缓冲保存退出原因，App_Close在资源释放后统一输出。
static char failure[192]{};

// 文件锁保证单进程访问设备；信号标志通知主循环执行统一收尾。
static int instance_lock{-1};
static volatile std::sig_atomic_t stop_requested{0};

static void request_stop_cb(int)
{
    // 信号回调只通知退出；线程和硬件留给正常流程关闭。
    stop_requested = 1;
}

/** @brief 记录不可继续的应用故障；返回 false 让 main 进入统一收尾。 */
static bool fail(const char *reason)
{
    std::snprintf(failure, sizeof(failure), "%s", reason);
    view.state = ui::DeviceUiState::Error;
    ui::show(view);
    return false;
}

/** @brief 唯一的业务状态写入口，同时把状态映射为 UI 快照。 */
static void set_state(AppState next)
{
    // 表项顺序对应AppState；WaitingReply在屏幕上显示为Thinking。
    const ui::DeviceUiState labels[] = {ui::DeviceUiState::Idle, ui::DeviceUiState::Listening,
                                        ui::DeviceUiState::Thinking,
                                        ui::DeviceUiState::Speaking};
    state = next;
    view.state =
        voice_net::online() ? labels[static_cast<unsigned>(next)] : ui::DeviceUiState::Offline;
    ui::show(view);
}

/** @brief 开启全新的监听窗口；与保留前滚的正常尾播追问路径区分。 */
static void begin_listening(Clock::duration window)
{
    // 新监听窗口从空前滚和零计数开始，截止时刻由传入的窗口长度决定。
    speech::reset();
    deadline = Clock::now() + window;
    set_state(AppState::Listening);
    debug::log.listening_started_cb();
}

/** @brief 先停本地输出，再退休网络轮次；在线时重新等开口，断线时回到待唤醒。
 * retract 要求服务端撤回中断的回答上下文，使后续问答从用户实际听到的内容继续。
 */
static void cancel_turn(bool retract)
{
    // 先停止扬声器并通知输入任务复位唤醒器，再让网络切换轮次。
    playback::cancel();
    voice_input::end_utterance();
    view.ClearText();
    if (voice_net::cancel(retract))
    {
        begin_listening(kFollowupWindow);
    }
    else
    {
        set_state(AppState::Idle);
    }
}

// 主线程按回复、输入、触摸的顺序处理；各动作实现在应用入口之后。
static void process_replies();
static void process_voice_frame(const audio::CaptureFrame &frame);
static void process_touch();

bool App_Init()
{
    failure[0] = '\0';
    view = {};
    stop_requested = 0;

    // 1. 文件锁保证声卡和屏幕由一个进程持有；信号处理函数负责发出退出通知。
    instance_lock = ::open("/run/boompi-client.lock", O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    if (instance_lock < 0)
    {
        return fail("cannot open instance lock");
    }
    if (flock(instance_lock, LOCK_EX | LOCK_NB) < 0)
    {
        return fail("client is already running or instance lock failed");
    }
    // Ctrl+C和进程停止请求都转成退出标志，实际清理由main调用App_Close完成。
    std::signal(SIGINT, request_stop_cb);
    std::signal(SIGTERM, request_stop_cb);
    // 连接断开由网络发送返回值报告，SIGPIPE交由该错误路径处理。
    std::signal(SIGPIPE, SIG_IGN);

    // 配置及网络使用会抛异常的标准库；此边界保证失败后 main 仍能调用 App_Close。
    try
    {
        // 2. 读配置，初始化界面和两路音频资源。
        config::VoiceClientConfig settings;
        std::string error;
        if (!config::LoadClientConfig(&settings, &error))
        {
            return fail(error.c_str());
        }
        view.volume = ui::load_volume();
        if (!ui::open())
        {
            // 界面故障通过日志提示，语音输入和播放仍按后续步骤初始化。
            debug::log.display_failed_cb("initialization");
        }
        if (!voice_input::open())
        {
            return fail(voice_input::error().c_str());
        }
        if (!playback::open(view.volume))
        {
            return fail(playback::error().c_str());
        }

        // 3. 两路 PCM 均已配置，才启动采集和网络，最后进入待唤醒。
        if (!voice_input::start())
        {
            return fail(voice_input::error().c_str());
        }
        if (!voice_net::open(settings))
        {
            return fail("network startup failed");
        }
        set_state(AppState::Idle);
        return true;
    }
    catch (const std::exception &)
    {
        return fail("application initialization failed");
    }
}

bool App_Process()
{
    if (stop_requested)
    {
        return false;
    }
    // 网络投递和错误字符串仍可能抛标准异常；普通业务失败由下面的返回值分支处理。
    try
    {
        // 1. 接收回复，再读取一帧；网络 I/O 和实际录音在各自线程推进。
        process_replies();
        audio::CaptureFrame frame;
        const auto input = voice_input::read(frame);
        if (input == voice_input::ReadResult::Failed)
        {
            return fail(voice_input::error().c_str());
        }

        // 2. 尾播真正完成后才开始追问，保留前滚中用户已经说出的部分。
        const auto output = playback::status();
        if (output == playback::State::Failed)
        {
            return fail(playback::error().c_str());
        }
        if (state == AppState::Speaking && output == playback::State::Drained)
        {
            debug::log.playback_done_cb();
            deadline = Clock::now() + kFollowupWindow;
            set_state(AppState::Listening);
        }

        // 3. 断点/超时优先结束当前语句；只有连续的有效帧才能进入唤醒和语句处理。
        const bool frame_ready = input == voice_input::ReadResult::Frame;
        // 仅Frame结果携带有效输入；20ms等待超时后主循环继续处理播放和触摸。
        const bool discontinuity = frame_ready && frame.discontinuity;
        if (discontinuity)
        {
            debug::log.input_discontinuity_cb();
        }
        if ((state != AppState::Idle && Clock::now() >= deadline) || discontinuity)
        {
            if (voice_net::uploading() || state == AppState::WaitingReply ||
                state == AppState::Speaking)
            {
                // 上传、等待回复、播放都属于活动轮次，超时或缺帧时一并取消。
                cancel_turn(state == AppState::Speaking);
            }
            else
            {
                set_state(AppState::Idle);
            }
        }
        else if (frame_ready)
        {
            process_voice_frame(frame);
        }

        // 4. 每轮最后处理触摸，使取消语句后的音量和按钮操作继续得到响应。
        process_touch();
        return true;
    }
    catch (const std::exception &)
    {
        return fail("application processing failed");
    }
}

int App_Close()
{
    // 先停止新的网络交付，再停止音频任务；各 close 在 join 后才释放自身资源。
    voice_net::close();
    playback::close();
    voice_input::close();
    ui::close();
    if (instance_lock >= 0)
    {
        // 关闭文件描述符后内核释放单实例锁，下一次启动即可重新占有设备。
        ::close(instance_lock);
        instance_lock = -1;
    }
    if (failure[0])
    {
        debug::log.failure_cb(failure);
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}

/** @brief 每轮最多消费 16 个网络事件，给输入和触摸留出执行机会。
 * AUDIO 只交付播放器；DONE 只关闭其输入，Speaking 需等 Drained 才进入追问。
 */
static void process_replies()
{
    voice_net::LinkEvent event;
    for (unsigned count = 0; count < 16 && voice_net::poll(event); ++count)
    {
        // 连接事件先恢复待唤醒状态，使新连接从清空的显示和播放状态开始。
        if (event.kind == voice_net::LinkEventKind::Online ||
            event.kind == voice_net::LinkEventKind::Offline)
        {
            playback::cancel();
            view.ClearText();
            if (event.kind == voice_net::LinkEventKind::Offline)
            {
                voice_input::end_utterance();
                debug::log.offline_cb(event.data.c_str());
            }
            else
            {
                debug::log.network_ready_cb();
            }
            set_state(AppState::Idle);
            continue;
        }
        if (event.kind == voice_net::LinkEventKind::Error)
        {
            debug::log.reply_failed_cb(event.data.c_str());
            cancel_turn(state == AppState::Speaking);
            continue;
        }
        if (Clock::now() >= deadline)
        {
            cancel_turn(state == AppState::Speaking);
            continue;
        }
        // 到包说明回复仍在推进；续期同时受整轮回复总时限约束。
        deadline = std::min(Clock::now() + kReplyIdleTimeout, response_limit);
        if (event.kind == voice_net::LinkEventKind::Text)
        {
            // 文字增量先并入字幕窗口，再交付完整显示快照。
            view.AppendText(event.data);
            ui::show(view);
        }
        else if (event.kind == voice_net::LinkEventKind::Audio)
        {
            if (playback::write(event.data.data(), event.data.size()) !=
                playback::WriteResult::Queued)
            {
                cancel_turn(true);
            }
            else if (state == AppState::WaitingReply)
            {
                debug::log.reply_audio_started_cb();
                // 第一包音频标记回答开始，语句模块从此观察播放中的插话候选。
                speech::reset();
                set_state(AppState::Speaking);
            }
        }
        else if (event.kind == voice_net::LinkEventKind::Done)
        {
            debug::log.reply_done_cb();
            if (state == AppState::WaitingReply)
            {
                // 纯文本回复收到 DONE 后直接进入追问；音频回复由下方播放器负责收尾。
                begin_listening(kFollowupWindow);
            }
            else
            {
                // finish通知播放器输入已经结束，Drained事件由主循环继续观察。
                playback::finish();
                deadline = std::min(Clock::now() + kDrainTimeout, response_limit);
            }
        }
    }
}

/** @brief 连续输入帧的业务处理：待唤醒 → 语句确认 → START → PCM → END。
 * 只在主线程消费传入帧，不复制 PCM、不创建任务；断点和超时由 App_Process 先处理。
 * 发送失败结束当前语句，控制返回主循环，随后继续处理触摸及连接状态。
 */
static void process_voice_frame(const audio::CaptureFrame &frame)
{
    if (!voice_net::online())
    {
        return;
    }
    if (state == AppState::Idle)
    {
        // 唤醒帧开启监听窗口，后续帧开始参与语句确认和句首缓存。
        if (frame.wake)
        {
            begin_listening(kWakeWindow);
        }
        return;
    }
    if (state == AppState::WaitingReply)
    {
        // 输入任务持续采集；等待回复阶段在应用层暂缓语句判断。
        return;
    }

    // Listening 和 Speaking 共用语句处理；只有播放中确认的新句才替换旧回答。
    const bool replacing = state == AppState::Speaking;
    const auto utterance = speech::update(frame, replacing && view.volume != 0);
    if (utterance.start && replacing)
    {
        playback::cancel();  // 插话确认后立即丢弃原回答的队列与声卡剩余数据。
    }
    else
    {
        playback::hold(utterance.hold_playback);
    }
    if (utterance.start)
    {
        // START成功后网络进入上传态，这时才交付语句模块返回的前滚和当前帧。
        if (voice_net::start(replacing) != voice_net::SendResult::Ok)
        {
            cancel_turn(false);
            return;
        }
        view.ClearText();
        deadline = Clock::now() + kUploadLimit;
        set_state(AppState::Listening);
        debug::log.upload_started_cb(static_cast<unsigned>(utterance.count) * audio::kFrameMs);
    }
    if (!voice_net::uploading())
    {
        // 等待开口或插话复核期间，PCM由speech前滚保存，后续确认时统一交付。
        return;
    }

    // START 的这一批包含前滚和当前帧；后续每次只有实时帧，句尾帧也先发送再 END。
    for (std::size_t i = 0; i < utterance.count; ++i)
    {
        if (voice_net::send(*utterance.frames[i]) != voice_net::SendResult::Ok)
        {
            cancel_turn(false);
            return;
        }
    }
    if (!utterance.end)
    {
        return;
    }
    if (voice_net::end() != voice_net::SendResult::Ok)
    {
        cancel_turn(false);
        return;
    }
    voice_input::end_utterance();
    deadline = Clock::now() + kReplyIdleTimeout;
    response_limit = Clock::now() + kReplyTotalLimit;
    set_state(AppState::WaitingReply);
    debug::log.upload_ended_cb();
}

/** @brief 消费一个触摸意图；页面只发出动作，是否执行由这里的实时业务状态决定。 */
static void process_touch()
{
    ui::UiAction action;
    if (!ui::poll_action(action))
    {
        return;
    }
    if (action.kind == ui::UiActionKind::Volume)
    {
        // 同一音量同时用于播放增益和界面快照，播放线程在后续块应用新值。
        view.volume = std::min<std::uint8_t>(100, action.volume);
        playback::set_volume(view.volume);
        ui::show(view);
    }
    else if (action.kind == ui::UiActionKind::Interrupt)
    {
        if (state == AppState::Speaking || state == AppState::WaitingReply)
        {
            cancel_turn(true);
        }
    }
    else if (state == AppState::Idle && voice_net::online())
    {
        begin_listening(kWakeWindow);
    }
}
