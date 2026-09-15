#pragma once

#include <cstdint>

#include "boompi/ui/ui_view.h"

namespace boompi::ui {

// 应用只交付显示快照、取走用户动作。LVGL、触摸、配网和摄像头均由 UI 线程管理。
class DeviceUi final {
 public:
  DeviceUi() noexcept = default;
  ~DeviceUi() noexcept;
  DeviceUi(const DeviceUi&) = delete;
  DeviceUi& operator=(const DeviceUi&) = delete;

  // 在打开音频前读保存的音量；读取失败用 fallback，始终限制到 0～100。
  static std::uint8_t LoadVolume(std::uint8_t fallback = 60) noexcept;
  // 打开显示/触摸，启动 UI 线程并等字体/页面准备。失败时语音可独立继续。
  // 线程启动等待上限 2 秒，硬件初始化和 join 不计入；重复打开先 Close。
  bool Open();
  // 复制完整快照并唤醒 UI；旧快照可被新值覆盖，返回后参数即可复用。
  void Show(const UiView& view) noexcept;
  // 不等待；动作和音量各保留最新值，动作优先。空指针/无动作返回 false。
  bool PollAction(UiAction* action) noexcept;
  // UI 线程先停摄像头/配网、释放 LVGL；join 后再关闭硬件。允许重复调用。
  void Close() noexcept;

 private:
  struct Impl;
  Impl* impl_{nullptr};
};

}  // namespace boompi::ui
