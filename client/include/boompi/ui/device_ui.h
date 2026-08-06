#pragma once

#include <cstdint>
#include <string_view>

namespace boompi::ui {

enum class DeviceUiState : std::uint8_t {
  kIdle, kListening, kThinking, kSpeaking, kHappy, kOffline, kError,
  // response.done 后仍有尾播，需要继续允许触屏打断。
  kSpeakingTail,
};

enum class DeviceUiAction : std::uint8_t { kWake, kInterrupt };

// 板端 UI 的公开边界：调用方只发布状态，LVGL 对象始终由 UI 线程独占。
class DeviceUi final {
 public:
  DeviceUi() noexcept = default;
  ~DeviceUi() noexcept;
  DeviceUi(const DeviceUi&) = delete;
  DeviceUi& operator=(const DeviceUi&) = delete;

  static std::uint8_t LoadVolume(std::uint8_t fallback) noexcept;
  bool Open() noexcept;
  void SetState(DeviceUiState state) noexcept;
  void SetText(std::string_view first_line, std::string_view second_line) noexcept;
  bool PollAction(DeviceUiAction* action) noexcept;
  bool PollVolumeChange(std::uint8_t* percent) noexcept;
  void SetVolume(std::uint8_t percent) noexcept;
  void Close() noexcept;

 private:
  struct Impl;
  Impl* impl_{nullptr};
};

}  // namespace boompi::ui
