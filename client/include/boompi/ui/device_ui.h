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
  kWake, kInterrupt,
};

/// 教学版异步 UI：状态更新只唤醒内部刷新线程；硬件不可用时静默降级。
/// 对象生命周期仍由调用线程独占，Close 不得与其他成员函数并发调用。
class DeviceUi final {
 public:
  DeviceUi() noexcept = default;
  ~DeviceUi() noexcept;
  DeviceUi(const DeviceUi&) = delete;
  DeviceUi& operator=(const DeviceUi&) = delete;

  // 仅在客户端启动阶段读取一次；运行时写盘始终留在 UI worker。
  static std::uint8_t LoadVolume(std::uint8_t fallback) noexcept;
  bool Open() noexcept;
  void SetState(DeviceUiState state) noexcept;
  void SetText(std::string_view first_line,
               std::string_view second_line) noexcept;
  // 点击语音页负责唤醒或打断；音量由页内可见滑块调节。
  bool PollAction(DeviceUiAction* action) noexcept;
  // 滑块预览值会合并；提交值由 UI worker 异步保存，application 只更新增益。
  bool PollVolumeChange(std::uint8_t* percent) noexcept;
  void SetVolume(std::uint8_t percent) noexcept;
  void Close() noexcept;

 private:
  struct Impl;
  Impl* impl_{nullptr};
};

}  // namespace boompi::ui

#endif  // BOOMPI_UI_DEVICE_UI_H_
