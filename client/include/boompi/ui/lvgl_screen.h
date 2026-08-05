#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include <lvgl.h>

#include "boompi/ui/device_ui.h"

namespace boompi::ui {

// Shared 320x240 product shell. The simulator and RV1106 port only provide
// display/input drivers; launcher navigation and app pages stay identical.
class LvglScreen final {
 public:
  enum class VolumeChangePhase : std::uint8_t {
    kBegin,
    kPreview,
    kCommit,
  };

  using VoiceInterruptHandler = void (*)(void* user_data);
  using AppActionHandler = void (*)(std::size_t app_index, void* user_data);
  using CameraActivityHandler = void (*)(bool active, void* user_data);
  using VolumeChangeHandler = void (*)(std::uint8_t percent,
                                       VolumeChangePhase phase,
                                       void* user_data);

  bool Create(const char* font_path, const char* icon_font_path) noexcept;
  void OpenApp(std::size_t index) noexcept;
  void SetVoiceInterruptHandler(VoiceInterruptHandler handler,
                                void* user_data) noexcept;
  void SetAppActionHandler(AppActionHandler handler, void* user_data) noexcept;
  void SetCameraActivityHandler(CameraActivityHandler handler,
                                void* user_data) noexcept;
  void SetVolumeChangeHandler(VolumeChangeHandler handler,
                              void* user_data) noexcept;
  void SetVolume(std::uint8_t percent) noexcept;
  void SetCameraFrame(const std::uint16_t* pixels,
                      std::size_t pixel_count) noexcept;
  void SetState(DeviceUiState state) noexcept;
  void SetText(std::string_view first, std::string_view second) noexcept;
  void Destroy() noexcept;

 private:
  enum class Page : std::uint8_t {
    kHome, kVoice, kCamera, kWifiProvision, kGeneric
  };
  static constexpr std::size_t kAppCount = 7;
  static constexpr std::size_t kCameraWidth = 320;
  static constexpr std::size_t kCameraHeight = 180;

  void BuildHome(lv_obj_t* root) noexcept;
  void BuildVoice(lv_obj_t* root) noexcept;
  void BuildCamera(lv_obj_t* root) noexcept;
  void BuildWifiProvision(lv_obj_t* root) noexcept;
  void BuildGeneric(lv_obj_t* root) noexcept;
  void ShowHome() noexcept;
  void ShowApp(std::size_t index) noexcept;
  void UpdateClock() noexcept;
  void UpdateVoice() noexcept;

  static void AppClicked(lv_event_t* event);
  static void BackClicked(lv_event_t* event);
  static void MuteClicked(lv_event_t* event);
  static void VolumeChanged(lv_event_t* event);
  static void VoiceClicked(lv_event_t* event);
  static void AppActionClicked(lv_event_t* event);
  static void ClockTick(lv_timer_t* timer);

  lv_obj_t* home_{nullptr};
  lv_obj_t* voice_{nullptr};
  lv_obj_t* camera_{nullptr};
  lv_obj_t* wifi_provision_{nullptr};
  lv_obj_t* generic_{nullptr};
  lv_obj_t* home_clock_{nullptr};
  lv_obj_t* home_date_{nullptr};
  lv_obj_t* home_year_{nullptr};
  lv_obj_t* home_face_image_{nullptr};
  lv_obj_t* app_buttons_[kAppCount]{};
  lv_obj_t* voice_face_image_{nullptr};
  lv_obj_t* voice_status_{nullptr};
  lv_obj_t* voice_text_{nullptr};
  lv_obj_t* voice_volume_slider_{nullptr};
  lv_obj_t* voice_volume_value_{nullptr};
  lv_obj_t* camera_image_{nullptr};
  lv_obj_t* camera_status_{nullptr};
  lv_obj_t* voice_mute_icon_{nullptr};
  lv_obj_t* generic_title_{nullptr};
  lv_obj_t* generic_hero_{nullptr};
  lv_obj_t* generic_badge_{nullptr};
  lv_obj_t* generic_icon_{nullptr};
  lv_obj_t* generic_primary_{nullptr};
  lv_obj_t* generic_secondary_{nullptr};
  lv_obj_t* generic_action_{nullptr};
  lv_obj_t* generic_stat_titles_[3]{};
  lv_obj_t* generic_stat_values_[3]{};
  lv_timer_t* clock_timer_{nullptr};

  lv_font_t* small_font_{nullptr};
  lv_font_t* home_caption_font_{nullptr};
  lv_font_t* desktop_font_{nullptr};
  lv_font_t* body_font_{nullptr};
  lv_font_t* title_font_{nullptr};
  lv_font_t* clock_font_{nullptr};
  lv_font_t* icon_font_{nullptr};
  lv_font_t* icon_small_font_{nullptr};
  bool freetype_initialized_{false};
  bool has_text_{false};
  bool muted_{false};
  std::uint8_t volume_percent_{60U};
  std::uint8_t volume_before_mute_{60U};
  VoiceInterruptHandler interrupt_handler_{nullptr};
  void* interrupt_user_data_{nullptr};
  AppActionHandler app_action_handler_{nullptr};
  void* app_action_user_data_{nullptr};
  CameraActivityHandler camera_activity_handler_{nullptr};
  void* camera_activity_user_data_{nullptr};
  VolumeChangeHandler volume_change_handler_{nullptr};
  void* volume_change_user_data_{nullptr};
  lv_img_dsc_t camera_descriptor_{};
  std::array<std::uint16_t, kCameraWidth * kCameraHeight> camera_pixels_{};
  std::size_t current_app_{0};
  Page page_{Page::kHome};
  DeviceUiState state_{DeviceUiState::kIdle};
};

}  // namespace boompi::ui
