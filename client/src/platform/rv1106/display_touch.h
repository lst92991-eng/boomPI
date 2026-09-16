/** @file display_touch.h
 * @brief ST7789P3 显示与 GT911 触摸的板端端口。
 *
 * 下行：LVGL flush_cb → Flush() → 旋转 RGB565 → SPI 面板；上行：read_cb →
 * ReadInput() → GT911 I2C 坐标 → 逻辑横屏点击。物理连线和恢复节拍集中在实现文件。
 */
#pragma once

#include <lvgl.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>

namespace display_touch
{

/**
 * @brief 同步 Linux 设备端口，不创建线程或持有业务状态。
 *
 * ui::open在调用线程完成硬件Open后才启动UI线程，join后才Close；运行期
 * Flush/ReadInput由UI线程串行执行，文件描述符的初始化、使用、释放依次进行。
 */
class DisplayTouch final
{
   public:
    DisplayTouch() = default;
    ~DisplayTouch()
    {
        Close();
    }
    DisplayTouch(const DisplayTouch &) = delete;
    DisplayTouch &operator=(const DisplayTouch &) = delete;

    /**
   * @brief 打开 SPI、GPIO 和 I2C，按板级时序初始化面板及触摸。
   * @return 任一步失败返回 false 并关闭已取得的 fd；成功只表示硬件初始化完成。
   *
   * 包含复位等待，必须在实时音频路径之外调用；重复调用会先关闭原有资源。
   */
    bool Open();
    /** @brief 先熄背光再关闭 fd、清触摸状态；允许重复调用，但需先停止所有 UI 回调。 */
    void Close();
    /**
   * @brief 将逻辑横屏脏矩形旋转写入面板，失败关背光并返回 false。
   * @param area LVGL 给出的有效 320x240 范围内闭区间矩形，不在本层再次裁剪。
   * @param pixels 按矩形行优先排列的 RGB565 像素，数量须覆盖整个 area。
   *
   * 同步消费像素，不保留指针；调用方仍须确认 lv_disp_flush_ready() 并决定是否退出。
   */
    bool Flush(const lv_area_t &area, const lv_color_t *pixels);
    /**
   * @brief 轮询 GT911 并填入横屏坐标及按压状态，data 必须非空。
   *
   * 无新数据时沿用已有点状态，读写失败释放按压并触发本端口的有界恢复。
   */
    void ReadInput(lv_indev_data_t *data);

   private:
    /** @brief D/C=0 发送命令字节；有 payload 时 D/C=1，返回值合并 GPIO/SPI 成功情况。 */
    bool Command(std::uint8_t command, const std::uint8_t *data = nullptr,
                 std::size_t bytes = 0U);
    /** @brief 复位面板并应用当前模组初始化表，最后才打开背光。 */
    bool InitPanel();
    /** @brief 使用 RESET/INT 选择 GT911 地址并试读，初始打开和故障恢复共用此时序。 */
    bool InitTouch();
    /** @brief 以 16 位寄存器地址执行组合 I2C 读；data/size 由本端口固定寄存器调用保证。 */
    bool ReadTouch(std::uint16_t address, std::uint8_t *data, std::size_t size);
    /** @brief 处理 ready 坐标后写零确认状态寄存器，让控制器继续提供新点。 */
    bool ClearTouchStatus();
    /** @brief 累积连续故障，按恢复预算重置触摸，耗尽后只禁用输入。 */
    void TouchFailed(const char *stage);
    /** @brief 只消费第一触点，维护下一次 ReadInput 输出的坐标和按压缓存。 */
    void PollTouch();

    // -1 表示未取得/已关闭；触摸 RESET/INT 的短期 fd 则在 InitTouch 内使用并关闭。
    int spi{-1}, touch{-1}, data_command{-1}, panel_reset{-1}, backlight{-1};
    int pointer_x{0}, pointer_y{0};
    bool pointer_pressed{false};
    unsigned touch_failures{0U}, touch_recovery_attempts{0U};
    bool touch_disabled{false};
    std::chrono::steady_clock::time_point next_touch_recovery{};
    // 以字节为单位的固定 SPI 分块缓冲，和 LVGL 的 32 行绘制缓冲分属两个阶段。
    std::array<std::uint8_t, 4096> flush_pixels{};
};

}  // namespace display_touch
