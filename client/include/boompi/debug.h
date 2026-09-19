/** @file debug.h
 * @brief 终端日志的只读回调表；调用者传事件及参数，格式与输出位置在 debug.cpp。
 * 回调在普通线程内同步消费参数并打印消息；业务状态由调用模块管理。
 */
#pragma once
#include <cstddef>
#include "boompi/audio/audio_frames.h"

namespace debug
{
// debug.cpp在进入main前注册全部回调，运行期只读；业务只交付事件和必要参数。
/** @brief 按进程、问答、音频、界面分类的日志入口；所有字段在 main 前一次初始化。 */
struct Callbacks
{
    // 正常对话节点：终端按这些事件观察连接、输入、上传和播放进度。
    void (*network_ready_cb)();
    void (*wake_detected_cb)();
    void (*vad_changed_cb)(audio::VoiceActivity activity);
    void (*listening_started_cb)();
    void (*upload_started_cb)(unsigned pre_roll_ms);
    void (*upload_ended_cb)();
    void (*reply_audio_started_cb)();
    void (*reply_done_cb)();
    void (*playback_done_cb)();

    // 故障及硬件事件携带发生阶段或必要数值，帮助定位处理链中的具体位置。
    void (*failure_cb)(const char *reason);
    void (*offline_cb)(const char *stage);
    void (*reply_failed_cb)(const char *code);
    void (*display_failed_cb)(const char *stage);
    void (*input_discontinuity_cb)();
    void (*barge_confirmed_cb)();
    void (*a3_initialized_cb)(bool success, unsigned elapsed_ms);
    void (*playback_xrun_cb)(std::size_t written_frames, int code);
    void (*volume_save_failed_cb)();
    void (*priority_failed_cb)(const char *thread, int priority, int code);
    void (*display_ready_cb)(unsigned spi_hz);
    void (*touch_disabled_cb)();
    void (*touch_recovery_cb)(const char *stage, unsigned attempt, unsigned limit);
    void (*touch_recovered_cb)();
};
extern const Callbacks log;
}  // namespace debug
