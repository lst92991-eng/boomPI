#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "boompi/ui/ui_view.h"

namespace boompi::ui {

// 模拟器与真板复用的页面；不操作硬件。全部方法和回调由同一个 LVGL 线程执行。
class LvglScreen final {
 public:
  enum class Page : std::uint8_t { Home, Voice, Camera, Clock, Wifi };
  enum class Event : std::uint8_t {
    Wake,
    Interrupt,
    Provision,
    CameraOn,
    CameraOff,
    VolumePreview,
    VolumeCommit,
  };
  // 回调同步执行；data 由宿主持有，value 仅用于音量百分比。
  using EventHandler = void (*)(Event event, std::uint8_t value, void* data);

  // 调用前完成 lv_init 和 display 注册；加载中文字库后创建桌面。
  // 成功后才能调用其余接口；重复创建先 Destroy，无自动析构清理。
  bool Create(const char* font_path) noexcept;
  // 程序化导航仍产生 CameraOn/Off，但不附带桌面点击的 Wake/Provision。
  void OpenApp(Page page) noexcept;
  // handler 为空时解除回调，data 借用到解除或 Destroy 完成。
  void SetEventHandler(EventHandler handler, void* data) noexcept;
  void SetVolume(std::uint8_t percent) noexcept;
  // 保存状态；活跃语音从桌面自动进入小智页，不抢占其他应用页面。
  void SetState(DeviceUiState state) noexcept;
  // 复制整段字幕；过滤缺失字形，空文本/分配失败时显示默认提示。
  void SetText(std::string_view text) noexcept;
  // 同步复制配网结果；离开配网页后忽略，error 决定文字颜色。
  void SetProvisionMessage(const char* text, bool error) noexcept;
  // 非 Live 状态隐藏图像，清除旧帧与帧率，防止误把冻结图像当实时预览。
  void SetCameraStatus(CameraStatus status) noexcept;
  // 同步复制 320×180 RGB565，count 必须匹配；fps_tenths 单位为 0.1 FPS。
  // 调用后上游缓冲即可复用，发布图像前应先设置 Live 状态。
  void SetCameraFrame(const std::uint16_t* pixels, std::size_t count,
                      unsigned fps_tenths) noexcept;
  // 先停宿主的摄像头，再释放定时器、页面和字体，最后才能关闭 display。
  // 不产生 CameraOff；允许重复调用。
  void Destroy() noexcept;

 private:
  struct Impl;
  Impl* impl_{nullptr};
};

}  // namespace boompi::ui
