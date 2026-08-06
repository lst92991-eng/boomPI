#include "boompi/ui/lvgl_screen.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <ctime>
#include <new>
#include <string>

#include <lvgl.h>

#include "twemoji_64.h"

namespace boompi::ui {
namespace {

constexpr std::uint32_t kBackground = 0x0A1220;
constexpr std::uint32_t kText = 0xF3F6FA;
constexpr std::uint32_t kMuted = 0x8492A6;
constexpr std::uint32_t kBlue = 0x4DA6FF;
constexpr std::array<const char*, 7> kApps{{
    "小智", "摄像头", "时间", "WiFi", "天气", "资源", "YOLO"}};

struct VoiceView final {
  const char* title;
  const char* hint;
  const lv_img_dsc_t* face;
};

const std::array<VoiceView, 8> kVoiceViews{{
    {"随时可以说话", "说出唤醒词开始对话", &emoji_1f642_64},
    {"正在聆听", "我在听，请继续", &emoji_1f62f_64},
    {"正在思考", "正在组织回答", &emoji_1f914_64},
    {"正在回答", "轻触表情即可打断", &emoji_1f606_64},
    {"回答完成", "随时可以继续问我", &emoji_1f642_64},
    {"暂时离线", "等待网络恢复", &emoji_1f614_64},
    {"发生错误", "请检查网络后重试", &emoji_1f614_64},
    {"即将说完", "轻触表情仍可打断", &emoji_1f606_64},
}};

constexpr std::array<const char*, 4> kCameraStatus{{
    "OFF", "START", "LIVE", "ERROR"}};

lv_obj_t* Label(lv_obj_t* parent, const char* value, const lv_font_t* font,
                std::uint32_t color, int x, int y, int width, int height) {
  lv_obj_t* label = lv_label_create(parent);
  lv_label_set_text(label, value);
  lv_obj_set_style_text_font(label, font, 0);
  lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
  lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_pos(label, x, y);
  lv_obj_set_size(label, width, height);
  return label;
}

bool HasGlyph(const lv_font_t* font, std::uint32_t codepoint) {
  lv_font_glyph_dsc_t glyph{};
  return lv_font_get_glyph_dsc(font, &glyph, codepoint, 0U) &&
         glyph.resolved_font != nullptr;
}

std::string SupportedText(const lv_font_t* font, const std::string& input) {
  std::string output;
  for (std::uint32_t offset = 0U; offset < input.size();) {
    const std::uint32_t start = offset;
    const std::uint32_t codepoint = _lv_txt_encoded_next(input.c_str(), &offset);
    if (codepoint == '\n' || (codepoint >= 0x20U && HasGlyph(font, codepoint)))
      output.append(input, start, offset - start);
  }
  return output;
}

lv_font_t* LoadFont(const char* path, std::uint16_t size) {
  lv_ft_info_t info{};
  info.name = path;
  info.weight = size;
  info.style = FT_FONT_STYLE_NORMAL;
  return lv_ft_font_init(&info) ? info.font : nullptr;
}

}  // namespace

struct LvglScreen::Impl final {
  enum class Page : std::uint8_t { kHome, kVoice, kCamera, kOther };
  static constexpr std::size_t kPixelCount = 320U * 180U;

  struct AppClick final {
    Impl* owner;
    std::size_t index;
  };

  std::array<AppClick, kApps.size()> app_clicks{};
  lv_obj_t* clock{};
  lv_obj_t* face{};
  lv_obj_t* subtitle{};
  lv_obj_t* slider{};
  lv_obj_t* provision_info{};
  lv_obj_t* camera_image{};
  lv_obj_t* camera_info{};
  lv_timer_t* timer{};
  lv_font_t* text_font{};
  EventHandler event_handler{};
  void* event_data{};
  lv_img_dsc_t camera_descriptor{};
  std::array<std::uint16_t, kPixelCount> pixels{};
  std::string subtitle_text;
  Page page{Page::kHome};
  DeviceUiState voice_state{DeviceUiState::kIdle};
  CameraStatus camera_state{CameraStatus::kStopped};
  unsigned fps_tenths{};
  std::uint8_t volume{60U};

  void Emit(Event event, std::uint8_t value = 0U) {
    if (event_handler != nullptr) event_handler(event, value, event_data);
  }

  void Begin(Page next) {
    lv_obj_clean(lv_scr_act());
    provision_info = nullptr;
    lv_obj_set_style_bg_color(lv_scr_act(), lv_color_hex(kBackground), 0);
    lv_obj_set_style_bg_opa(lv_scr_act(), LV_OPA_COVER, 0);
    page = next;
  }

  void Header(const char* title) {
    lv_obj_t* back = Label(lv_scr_act(), "<", text_font, kText, 0, 0, 44, 38);
    lv_obj_set_style_pad_top(back, 7, 0);
    lv_obj_add_flag(back, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(back, BackClicked, LV_EVENT_CLICKED, this);
    Label(lv_scr_act(), title, text_font, kText, 100, 8, 120, 24);
  }

  void BuildHome() {
    Begin(Page::kHome);
    Label(lv_scr_act(), "boomPI", text_font, kText, 10, 8, 80, 24);
    clock = Label(lv_scr_act(), "", text_font, kText, 235, 8, 70, 24);
    for (std::size_t index = 0U; index < kApps.size(); ++index) {
      const int x = 5 + static_cast<int>(index % 4U) * 79;
      const int y = 43 + static_cast<int>(index / 4U) * 94;
      lv_obj_t* button = lv_btn_create(lv_scr_act());
      lv_obj_set_pos(button, x, y);
      lv_obj_set_size(button, 72, 86);
      app_clicks[index] = {this, index};
      lv_obj_add_event_cb(button, AppClicked, LV_EVENT_CLICKED, &app_clicks[index]);
      Label(button, kApps[index], text_font, kText, 2, 31, 68, 24);
    }
    UpdateClock();
  }

  void BuildVoice() {
    Begin(Page::kVoice);
    Header("小智");
    face = lv_img_create(lv_scr_act());
    lv_img_set_zoom(face, 328);
    lv_obj_set_pos(face, 119, 45);
    lv_obj_add_flag(face, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(face, FaceClicked, LV_EVENT_CLICKED, this);
    subtitle = Label(lv_scr_act(), "", text_font, kMuted, 17, 143, 286, 67);
    lv_label_set_long_mode(subtitle, LV_LABEL_LONG_WRAP);
    slider = lv_slider_create(lv_scr_act());
    lv_obj_set_pos(slider, 35, 222);
    lv_obj_set_size(slider, 215, 7);
    lv_slider_set_range(slider, 0, 100);
    lv_obj_add_event_cb(slider, SliderChanged, LV_EVENT_ALL, this);
    Label(lv_scr_act(), "音量", text_font, kMuted, 0, 214, 36, 22);
    SetVolume(volume);
    RenderVoice();
  }

  void BuildCamera() {
    Begin(Page::kCamera);
    camera_state = CameraStatus::kStarting;
    Header("SC3336");
    camera_info = Label(lv_scr_act(), "", text_font, kBlue, 160, 7, 152, 24);
    camera_image = lv_img_create(lv_scr_act());
    lv_img_set_src(camera_image, &camera_descriptor);
    lv_obj_set_pos(camera_image, 0, 30);
    RenderCamera();
  }

  void BuildOther(std::size_t index) {
    Begin(Page::kOther);
    Header(kApps[index]);
    if (index == 3U) {
      lv_obj_t* qr = lv_qrcode_create(lv_scr_act(), 104, lv_color_hex(0x08111C),
                                      lv_color_hex(0xFFFFFF));
      constexpr char payload[] = "WIFI:T:WPA;S:boomPI-Setup;P:boompi-setup;;";
      lv_obj_set_pos(qr, 108, 48);
      lv_qrcode_update(qr, payload, sizeof(payload) - 1U);
      Label(lv_scr_act(), "boomPI-Setup", text_font, kText, 60, 166, 200, 24);
      provision_info = Label(lv_scr_act(), "扫描二维码连接配网热点",
                             text_font, kMuted, 30, 197, 260, 24);
      return;
    }
    Label(lv_scr_act(), "功能将在后续课程接入", text_font, kMuted,
          30, 105, 260, 30);
  }

  void ShowApp(std::size_t index) {
    if (index >= kApps.size()) return;
    const bool was_camera = page == Page::kCamera;
    if (index == 0U)
      BuildVoice();
    else if (index == 1U)
      BuildCamera();
    else
      BuildOther(index);
    if (was_camera && index != 1U) Emit(Event::kCameraOff);
    if (index == 1U) Emit(Event::kCameraOn);
  }

  void SetVolume(std::uint8_t percent) {
    volume = std::min<std::uint8_t>(percent, 100U);
    if (page != Page::kVoice) return;
    lv_slider_set_value(slider, volume, LV_ANIM_OFF);
  }

  void RenderVoice() {
    if (page != Page::kVoice) return;
    const VoiceView& view = kVoiceViews[static_cast<std::size_t>(voice_state)];
    lv_img_set_src(face, view.face);
    lv_label_set_text_fmt(subtitle, "%s\n%s", view.title,
                          subtitle_text.empty() ? view.hint : subtitle_text.c_str());
  }

  void RenderCamera() {
    if (page != Page::kCamera) return;
    if (camera_state == CameraStatus::kLive)
      lv_label_set_text_fmt(camera_info, "LIVE %u.%u FPS",
                            fps_tenths / 10U, fps_tenths % 10U);
    else
      lv_label_set_text(camera_info,
                        kCameraStatus[static_cast<std::size_t>(camera_state)]);
    lv_img_cache_invalidate_src(&camera_descriptor);
    lv_obj_invalidate(camera_image);
  }

  void UpdateClock() {
    if (page != Page::kHome) return;
    const std::time_t now = std::time(nullptr);
    std::tm local{};
    localtime_r(&now, &local);
    lv_label_set_text_fmt(clock, "%02d:%02d", local.tm_hour, local.tm_min);
  }

  static void BackClicked(lv_event_t* event) {
    auto* self = static_cast<Impl*>(lv_event_get_user_data(event));
    const bool was_camera = self->page == Page::kCamera;
    self->BuildHome();
    if (was_camera) self->Emit(Event::kCameraOff);
  }

  static void AppClicked(lv_event_t* event) {
    auto* click = static_cast<AppClick*>(lv_event_get_user_data(event));
    click->owner->ShowApp(click->index);
    if (click->index == 0U) click->owner->Emit(Event::kWake);
    if (click->index == 3U) click->owner->Emit(Event::kProvision);
  }

  static void FaceClicked(lv_event_t* event) {
    Impl* self = static_cast<Impl*>(lv_event_get_user_data(event));
    const bool speaking = self->voice_state == DeviceUiState::kSpeaking ||
                          self->voice_state == DeviceUiState::kSpeakingTail;
    if (speaking) self->Emit(Event::kInterrupt);
  }

  static void SliderChanged(lv_event_t* event) {
    const lv_event_code_t code = lv_event_get_code(event);
    const bool committed = code == LV_EVENT_RELEASED ||
                           code == LV_EVENT_PRESS_LOST;
    if (code != LV_EVENT_VALUE_CHANGED && !committed) return;
    Impl* self = static_cast<Impl*>(lv_event_get_user_data(event));
    self->SetVolume(static_cast<std::uint8_t>(lv_slider_get_value(self->slider)));
    const Event change = committed ? Event::kVolumeCommit
                                   : Event::kVolumePreview;
    self->Emit(change, self->volume);
  }
};

bool LvglScreen::Create(const char* font_path) noexcept {
  Destroy();
  impl_ = new (std::nothrow) Impl;
  if (impl_ == nullptr) return false;
  if (font_path == nullptr || !lv_freetype_init(2, 4, 65536)) {
    delete impl_;
    impl_ = nullptr;
    return false;
  }
  impl_->text_font = LoadFont(font_path, 16);
  if (impl_->text_font == nullptr || !HasGlyph(impl_->text_font, 0x667AU)) {
    Destroy();
    return false;
  }
  impl_->camera_descriptor.header.cf = LV_IMG_CF_TRUE_COLOR;
  impl_->camera_descriptor.header.w = 320;
  impl_->camera_descriptor.header.h = 180;
  impl_->camera_descriptor.data_size = impl_->pixels.size() * sizeof(std::uint16_t);
  impl_->camera_descriptor.data = reinterpret_cast<const std::uint8_t*>(
      impl_->pixels.data());
  impl_->timer = lv_timer_create([](lv_timer_t* timer) {
    static_cast<Impl*>(timer->user_data)->UpdateClock();
  }, 60000, impl_);
  impl_->BuildHome();
  return true;
}

void LvglScreen::OpenApp(std::size_t index) noexcept {
  impl_->ShowApp(index);
}

void LvglScreen::SetEventHandler(EventHandler handler, void* data) noexcept {
  impl_->event_handler = handler;
  impl_->event_data = data;
}

void LvglScreen::SetVolume(std::uint8_t percent) noexcept {
  impl_->SetVolume(percent);
}

void LvglScreen::SetProvisionMessage(const char* text, bool error) noexcept {
  if (impl_ == nullptr || impl_->provision_info == nullptr) return;
  lv_label_set_text(impl_->provision_info, text);
  lv_obj_set_style_text_color(
      impl_->provision_info, lv_color_hex(error ? 0xFF6B6B : kMuted), 0);
}

void LvglScreen::SetCameraStatus(CameraStatus state) noexcept {
  if (impl_ == nullptr) return;
  impl_->camera_state = state;
  if (state != CameraStatus::kLive) {
    impl_->fps_tenths = 0U;
    impl_->pixels.fill(0U);
  }
  impl_->RenderCamera();
}

void LvglScreen::SetCameraFrame(const std::uint16_t* pixels, std::size_t pixel_count,
                                unsigned fps_tenths) noexcept {
  if (impl_ == nullptr || pixels == nullptr ||
      pixel_count != impl_->pixels.size()) return;
  std::memcpy(impl_->pixels.data(), pixels, pixel_count * sizeof(pixels[0]));
  impl_->fps_tenths = fps_tenths;
  impl_->RenderCamera();
}

void LvglScreen::SetState(DeviceUiState state) noexcept {
  if (impl_ == nullptr) return;
  impl_->voice_state = state;
  const bool active = (state >= DeviceUiState::kListening &&
                       state <= DeviceUiState::kHappy) ||
                      state == DeviceUiState::kSpeakingTail;
  if (active && impl_->page == Impl::Page::kHome)
    impl_->BuildVoice();
  impl_->RenderVoice();
}

void LvglScreen::SetText(std::string_view first, std::string_view second) noexcept {
  if (impl_ == nullptr) return;
  std::string text(first);
  if (!text.empty() && !second.empty()) text.push_back('\n');
  text.append(second);
  impl_->subtitle_text = SupportedText(impl_->text_font, text);
  impl_->RenderVoice();
}

void LvglScreen::Destroy() noexcept {
  if (impl_ == nullptr) return;
  if (impl_->timer != nullptr) lv_timer_del(impl_->timer);
  lv_obj_clean(lv_scr_act());
  if (impl_->text_font != nullptr) lv_ft_font_destroy(impl_->text_font);
  lv_freetype_destroy();
  delete impl_;
  impl_ = nullptr;
}

}  // namespace boompi::ui
