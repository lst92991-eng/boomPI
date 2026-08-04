#ifndef BOOMPI_UI_DEVICE_UI_H_
#define BOOMPI_UI_DEVICE_UI_H_

#include <cstdint>
#include <string>
#include <string_view>

namespace boompi::ui {

enum class DeviceUiState : std::uint8_t {
  kIdle, kListening, kThinking, kSpeaking, kHappy, kOffline, kError,
  // response.done 后仍有尾播：保持触屏打断，且与三秒追问状态区分。
  kSpeakingTail,
};

enum class DeviceUiAction : std::uint8_t {
  kWake, kInterrupt, kVolumeUp, kVolumeDown, kBrightnessUp, kBrightnessDown,
};

/// 教学版异步 UI：状态更新只唤醒内部刷新线程；硬件不可用时静默降级。
/// 对象生命周期仍由调用线程独占，Close 不得与其他成员函数并发调用。
class DeviceUi final {
 public:
  DeviceUi() noexcept = default; ~DeviceUi() noexcept;
  DeviceUi(const DeviceUi&) = delete; DeviceUi& operator=(const DeviceUi&) = delete;

  bool Open() noexcept;
  void SetState(DeviceUiState state) noexcept; void SetText(std::string_view first_line, std::string_view second_line) noexcept;
  // 点击为打断；横屏竖滑调音量，横滑调亮度。
  bool PollAction(DeviceUiAction* action) noexcept; void SetBrightness(std::uint8_t percent) noexcept;
  // 当前 BL 只有 GPIO 开关证据：0 表示关，其余值均表示开，不伪造 PWM 多级亮度。
  void Close() noexcept;

 private:
  struct Impl;
  Impl* impl_{nullptr};
};

}  // namespace boompi::ui

#endif  // BOOMPI_UI_DEVICE_UI_H_
