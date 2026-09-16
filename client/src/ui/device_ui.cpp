/** @file device_ui.cpp
 * @brief UI 生命周期与线程交接：硬件/页面初始化 → 快照刷新/触摸 → 停止回收。
 *
 * 主线程调用 show/poll_action；UI 线程执行 LVGL、SPI/I2C 和音量持久化。
 * view/dirty 在 mutex 下交接；用户动作和音量分别原子覆盖为最新值，不建立事件长队列。
 */
#include "boompi/ui/device_ui.h"

#include <fcntl.h>
#include <lvgl.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <exception>
#include <mutex>
#include <thread>

#include "../platform/rv1106/display_touch.h"
#include "boompi/debug.h"
#include "boompi/ui/lvgl_screen.h"

namespace ui
{
static const char kSettings[] = "/userdata/boompi/config/ui.settings";
static const char kTemporary[] = "/userdata/boompi/config/ui.settings.tmp";
static const char kFont[] = "/userdata/boompi/fonts/NotoSansCJK-Regular.ttc";
static const char kFallbackFont[] = "/oem/usr/share/simsun_en.ttf";
static display_touch::DisplayTouch hardware;
static std::thread worker;
static std::atomic<bool> stopping{false};
static std::atomic<int> action{-1}, volume{-1};
// -1表示没有待处理操作；开始/停止与音量分别保留最新值，交由主线程消费。
static std::mutex mutex;
static std::condition_variable changed;
static UiView view;
static bool dirty{true};
// dirty标记新快照已到达，UI线程完成一次复制后清除。
static lv_disp_t *display{};
static lv_indev_t *input{};
static lv_disp_draw_buf_t draw_buffer;
static lv_disp_drv_t output;
static lv_indev_drv_t pointer;
static std::array<lv_color_t, 320 * 32> draw_pixels;
// 绘制缓冲覆盖32行像素，LVGL逐块调用flush将脏区域交给SPI面板。
/** @brief 保存释放滑块时的最终音量；失败保留旧配置并报告，不中断语音业务。 */
static void save_volume(std::uint8_t value)
{
    // 仅在释放滑块时提交；临时文件完整写入后才替换正式配置。
    const int fd =
        ::open(kTemporary, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
    std::FILE *file = fd < 0 ? nullptr : fdopen(fd, "w");
    bool ok = file && std::fprintf(file, "%u\n", value) > 0 && std::fflush(file) == 0 &&
              fsync(fd) == 0;
    if (file)
    {
        ok = std::fclose(file) == 0 && ok;
    }
    else if (fd >= 0)
    {
        ::close(fd);
    }
    if (!ok || std::rename(kTemporary, kSettings) != 0)
    {
        unlink(kTemporary);
        debug::log.volume_save_failed_cb();
    }
}

/** @brief 页面回调只交付动作；音量保存留在 UI 线程，让应用主循环保持及时处理音频。 */
static void event_cb(ui_page::Event type, std::uint8_t value)
{
    if (type == ui_page::Event::Volume || type == ui_page::Event::SaveVolume)
    {
        // 拖动过程立即发布最新音量，释放滑块的事件再负责保存到文件。
        volume.store(value);
        if (type == ui_page::Event::SaveVolume)
        {
            save_volume(value);
        }
    }
    else
    {
        action.store(static_cast<int>(type == ui_page::Event::Wake ? UiActionKind::Wake
                                                                   : UiActionKind::Interrupt));
    }
}

/** @brief 同步刷脏矩形；失败请求 UI 停止，但仍必须通知 LVGL 本次 flush_cb 已结束。 */
static void flush_cb(lv_disp_drv_t *driver, const lv_area_t *area, lv_color_t *pixels)
{
    if (!hardware.Flush(*area, pixels))
    {
        if (!stopping.exchange(true))
        {
            debug::log.display_failed_cb("SPI write");
        }
    }
    lv_disp_flush_ready(driver);
}

/** @brief UI 任务：复制最新快照 → 更新控件 → 推进 LVGL → 等下一次刷新。
 * 字幕处理使用固定缓冲；设备 I/O 通过返回值报告失败，不用异常控制刷新循环。
 */
static void run()
{
    static_cast<void>(nice(5));
    auto tick = std::chrono::steady_clock::now();
    while (!stopping.load())
    {
        // 快照只在短锁内复制；渲染、字体和SPI不持应用交接锁。
        UiView next;
        bool update;
        {
            std::lock_guard<std::mutex> lock(mutex);
            update = dirty;
            if (update)
            {
                next = view;
            }
            dirty = false;
        }
        if (update)
        {
            ui_page::show(next);
        }
        const auto now = std::chrono::steady_clock::now();
        // 给LVGL补上本次循环经过的毫秒数，保证动画、触摸及内部定时器按真实时间推进。
        lv_tick_inc(static_cast<std::uint32_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(now - tick).count()));
        tick = now;
        lv_timer_handler();
        // 有新快照时可提前唤醒；30ms周期同时保证空闲时仍轮询触摸和刷新定时器。
        std::unique_lock<std::mutex> lock(mutex);
        changed.wait_for(lock, std::chrono::milliseconds(30),
                         []
                         {
                             return stopping.load() || dirty;
                         });
    }
}
std::uint8_t load_volume(std::uint8_t fallback)
{
    unsigned value = std::min<std::uint8_t>(fallback, 100);
    if (auto *file = std::fopen(kSettings, "r"))
    {
        unsigned saved;
        if (std::fscanf(file, "%u", &saved) == 1 && saved <= 100)
        {
            value = saved;
        }
        std::fclose(file);
    }
    return static_cast<std::uint8_t>(value);
}

bool open()
{
    close();
    if (!hardware.Open())
    {
        return false;
    }
    // 尚未启动工作线程，此处依次配置LVGL端口、字体和小智页面。
    lv_init();
    // 注册显示端口：分辨率描述逻辑横屏，flush负责把像素送到实际面板。
    lv_disp_draw_buf_init(&draw_buffer, draw_pixels.data(), nullptr, draw_pixels.size());
    lv_disp_drv_init(&output);
    output.hor_res = 320;
    output.ver_res = 240;
    output.draw_buf = &draw_buffer;
    output.flush_cb = flush_cb;
    display = lv_disp_drv_register(&output);
    // 注册触摸端口：read_cb提供坐标和按压状态，LVGL据此识别点击与拖动。
    lv_indev_drv_init(&pointer);
    pointer.type = LV_INDEV_TYPE_POINTER;
    pointer.read_cb = [](lv_indev_drv_t *, lv_indev_data_t *data)
    {
        hardware.ReadInput(data);
    };
    input = lv_indev_drv_register(&pointer);
    const char *font = access(kFont, R_OK) == 0 ? kFont : kFallbackFont;
    // 字体和页面建立后才启动UI线程，使线程进入循环时已有完整控件树。
    if (!display || !input || !ui_page::open(font, event_cb))
    {
        close();
        return false;
    }
    view = {};
    dirty = true;
    action.store(-1);
    volume.store(-1);
    stopping.store(false);
    // std::thread 创建失败会抛标准异常；只在这个边界回收已配置的 UI 资源。
    try
    {
        worker = std::thread(run);
        return true;
    }
    catch (const std::exception &)
    {
        close();
        return false;
    }
}

void show(const UiView &value)
{
    // 只在锁内复制小型快照，绘图和SPI传输由UI线程随后完成。
    std::lock_guard<std::mutex> lock(mutex);
    view = value;
    dirty = true;
    changed.notify_one();
}

bool poll_action(UiAction &result)
{
    // exchange读取并清空操作槽；开始/停止优先交付，音量留给下一次调用。
    const int command = action.exchange(-1);
    if (command >= 0)
    {
        result.kind = static_cast<UiActionKind>(command);
        return true;
    }
    const int percent = volume.exchange(-1);
    if (percent < 0)
    {
        return false;
    }
    result = {UiActionKind::Volume, static_cast<std::uint8_t>(percent)};
    return true;
}

void close()
{
    // 先唤醒并等待UI线程退出，再销毁它可能使用的控件、字体和设备端口。
    stopping.store(true);
    changed.notify_one();
    if (worker.joinable())
    {
        worker.join();
    }
    // join确认UI回调已经结束，再按页面、输入/显示端口、硬件的顺序释放资源。
    ui_page::close();
    if (input)
    {
        lv_indev_delete(input);
        input = nullptr;
    }
    if (display)
    {
        lv_disp_remove(display);
        display = nullptr;
    }
    // LVGL8.2的disp_remove不释放自动分配的draw_ctx，由端口持有者收尾。
    if (output.draw_ctx)
    {
        output.draw_ctx_deinit(&output, output.draw_ctx);
        lv_mem_free(output.draw_ctx);
        output.draw_ctx = nullptr;
    }
    hardware.Close();
}
}  // namespace ui
