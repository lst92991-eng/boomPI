/**
 * @file lvgl_screen.h
 * @brief 定义 SDL 模拟器与 RV1106 共用的纯 LVGL 页面控制接口。
 *
 * 页面层接收已经整理好的显示数据并发布用户意图，显示、触摸和摄像头硬件操作
 * 留在 DeviceUi。所有方法必须由同一个 LVGL 所有者线程调用。
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "boompi/ui/device_ui.h"

namespace boompi::ui {

/**
 * @brief 共享给 SDL 模拟器和 RV1106 显示端口的 LVGL 页面控制器。
 *
 * 本类只保存页面对象和渲染所需的小型状态，不打开 SPI、I2C、摄像头或配置文件。
 * 调用方负责把硬件事件转换为 Event，并保证 Create() 到 Destroy() 之间的所有
 * 方法都由同一个 LVGL 所有者线程调用。这个约束让模拟器与真机复用同一套页面，
 * 同时保持 LVGL 对象严格单线程访问。
 */
class LvglScreen final {
 public:
  /** @brief 摄像头采集链路向页面报告的生命周期阶段。 */
  enum class CameraStatus : std::uint8_t { kStopped, kStarting, kLive, kError };

  /**
   * @brief 页面产生的用户事件。
   *
   * kVolumePreview 只更新实时增益，kVolumeCommit 表示手指释放后可以持久化；
   * 摄像头和配网事件只表达意图，具体进程与设备资源由 DeviceUi 管理。
   */
  enum class Event : std::uint8_t {
    kWake, kInterrupt, kProvision, kCameraOn, kCameraOff, kVolumePreview,
    kVolumeCommit,
  };
  using EventHandler = void (*)(Event event, std::uint8_t value, void* data);

  /**
   * @brief 加载字体、建立页面资源并显示桌面。
   * @param font_path 同时覆盖中文和拉丁字符的 FreeType 字体文件。
   */
  bool Create(const char* font_path) noexcept;

  /** @brief 打开桌面索引对应的页面，主要供模拟器生成固定页面预览。 */
  void OpenApp(std::size_t index) noexcept;

  /** @brief 注册同步事件回调；回调运行在调用 LVGL handler 的同一线程。 */
  void SetEventHandler(EventHandler handler, void* data) noexcept;

  /** @brief 更新语音页音量滑块，输入值自动限制到 0..100。 */
  void SetVolume(std::uint8_t percent) noexcept;

  /** @brief 更新 Wi-Fi 二维码页的配网进程结果。 */
  void SetProvisionMessage(const char* text, bool error) noexcept;

  /** @brief 更新摄像头状态；离开 LIVE 时清除旧帧和帧率。 */
  void SetCameraStatus(CameraStatus status) noexcept;

  /**
   * @brief 提交一帧 320x180 RGB565 图像和实测帧率。
   * @param count 必须等于 320*180，防止页面读取尺寸不匹配的缓冲区。
   * @param fps_tenths 以 0.1 FPS 为单位，避免浮点格式化进入板端 UI。
   */
  void SetCameraFrame(const std::uint16_t* pixels, std::size_t count, unsigned fps_tenths) noexcept;

  /** @brief 更新语音表情；活跃状态可以自动从桌面进入小智页。 */
  void SetState(DeviceUiState state) noexcept;

  /** @brief 更新最多两段字幕，渲染前过滤字体不支持的 Unicode 字符。 */
  void SetText(std::string_view first, std::string_view second) noexcept;

  /** @brief 按 LVGL 依赖顺序释放定时器、页面、字体和 FreeType。 */
  void Destroy() noexcept;

 private:
  struct Impl;
  Impl* impl_{nullptr};
};

}  // namespace boompi::ui
