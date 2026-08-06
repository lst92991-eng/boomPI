#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "boompi/ui/device_ui.h"

namespace boompi::ui {

// Page controller shared by the simulator and the RV1106 display port.
class LvglScreen final {
 public:
  enum class CameraStatus : std::uint8_t { kStopped, kStarting, kLive, kError };
  enum class Event : std::uint8_t {
    kWake, kInterrupt, kProvision, kCameraOn, kCameraOff, kVolumePreview,
    kVolumeCommit,
  };
  using EventHandler = void (*)(Event event, std::uint8_t value, void* data);

  bool Create(const char* font_path) noexcept;
  void OpenApp(std::size_t index) noexcept;
  void SetEventHandler(EventHandler handler, void* data) noexcept;
  void SetVolume(std::uint8_t percent) noexcept;
  void SetProvisionMessage(const char* text, bool error) noexcept;
  void SetCameraStatus(CameraStatus status) noexcept;
  void SetCameraFrame(const std::uint16_t* pixels, std::size_t count, unsigned fps_tenths) noexcept;
  void SetState(DeviceUiState state) noexcept;
  void SetText(std::string_view first, std::string_view second) noexcept;
  void Destroy() noexcept;

 private:
  struct Impl;
  Impl* impl_{nullptr};
};

}  // namespace boompi::ui
