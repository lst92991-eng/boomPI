/** @file debug.cpp
 * @brief 集中定义终端日志文案，并在 main 前注册只读回调表。
 * 调用点负责选择事件；回调只输出，不发起取消、重连或其他业务动作。
 */
#include "boompi/debug.h"

#include <cstdio>

namespace debug
{
/** @brief 返回完整回调表；无捕获 lambda 可直接转为函数指针，无额外对象或消息队列。 */
static Callbacks register_callbacks()
{
    // 在进程进入main前绑定各事件的输出函数，运行时通过debug::log直接调用。
    // 所有文案写到stderr；前台终端直接显示，重定向后可由tail持续查看。
    Callbacks callbacks{};
    callbacks.network_ready_cb = []
    {
        std::fprintf(stderr, "boompi: 服务端握手完成，可以开始对话\n");
    };
    callbacks.wake_detected_cb = []
    {
        std::fprintf(stderr, "boompi: 检测到唤醒词\n");
    };
    callbacks.vad_changed_cb = [](bool speech)
    {
        std::fprintf(stderr, "boompi: VAD -> %s\n", speech ? "有人声" : "静音");
    };
    callbacks.listening_started_cb = []
    {
        std::fprintf(stderr, "boompi: 进入监听，等待用户开口\n");
    };
    callbacks.upload_started_cb = [](unsigned pre_roll_ms)
    {
        std::fprintf(stderr, "boompi: START已入队，补发%u毫秒句首音频\n", pre_roll_ms);
    };
    callbacks.upload_ended_cb = []
    {
        std::fprintf(stderr, "boompi: END已入队，等待回答\n");
    };
    callbacks.reply_audio_started_cb = []
    {
        std::fprintf(stderr, "boompi: 收到首包回答音频，交给播放器\n");
    };
    callbacks.reply_done_cb = []
    {
        std::fprintf(stderr, "boompi: 收到DONE，服务端已结束发送\n");
    };
    callbacks.playback_done_cb = []
    {
        std::fprintf(stderr, "boompi: 声卡尾音已播完，进入追问窗口\n");
    };
    callbacks.failure_cb = [](const char *reason)
    {
        std::fprintf(stderr, "boompi-client: %s\n", reason);
    };
    callbacks.offline_cb = [](const char *stage)
    {
        std::fprintf(stderr, "boompi: offline; stage=%s\n", stage);
    };
    callbacks.reply_failed_cb = [](const char *code)
    {
        std::fprintf(stderr, "boompi: reply failed; code=%s\n", code);
    };
    callbacks.display_failed_cb = [](const char *stage)
    {
        std::fprintf(stderr, "boompi: display failed; stage=%s; voice continues\n", stage);
    };
    callbacks.input_discontinuity_cb = []
    {
        std::fprintf(stderr, "boompi: input discontinuity; current input canceled\n");
    };
    callbacks.barge_probe_cb = []
    {
        std::fprintf(stderr, "boompi: 发现插话候选，请求短暂停播复核\n");
    };
    callbacks.barge_confirmed_cb = []
    {
        std::fprintf(stderr, "boompi: 插话复核通过，准备提交新语句\n");
    };
    callbacks.barge_rejected_cb = [](bool reference_active)
    {
        std::fprintf(stderr, "boompi: 插话复核未通过；reference=%d\n", reference_active);
    };
    callbacks.playback_xrun_cb = [](std::size_t written_frames, int code)
    {
        std::fprintf(stderr, "boompi: playback xrun after %zu frames (%d)\n", written_frames,
                     code);
    };
    callbacks.volume_save_failed_cb = []
    {
        std::fprintf(stderr, "boompi-ui: volume save failed\n");
    };
    callbacks.priority_failed_cb = [](const char *thread, int priority, int code)
    {
        std::fprintf(stderr, "boompi-client: warning: %s realtime priority %d failed (%d)\n",
                     thread, priority, code);
    };
    callbacks.display_ready_cb = [](unsigned spi_hz)
    {
        std::fprintf(stderr, "boompi-ui: SPI=%u Hz; touch=GT911/i2c-3\n", spi_hz);
    };
    callbacks.touch_disabled_cb = []
    {
        std::fprintf(stderr, "boompi-ui: GT911 disabled after bounded recovery\n");
    };
    callbacks.touch_recovery_cb = [](const char *stage, unsigned attempt, unsigned limit)
    {
        std::fprintf(stderr, "boompi-ui: GT911 %s failed; recovery %u/%u\n", stage, attempt,
                     limit);
    };
    callbacks.touch_recovered_cb = []
    {
        std::fprintf(stderr, "boompi-ui: GT911 recovered\n");
    };
    return callbacks;
}
// 每次回调同步写一条终端消息，不新增业务锁或保存参数；跨线程输出由stdio串行化。
const Callbacks log = register_callbacks();
}  // namespace debug
