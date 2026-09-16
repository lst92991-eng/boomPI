#include "boompi/ui/lvgl_screen.h"
#include <lvgl.h>
#include <algorithm>
#include <cstring>
#include <string>
#include "twemoji_64.h"

namespace boompi::ui::page {
namespace {
lv_obj_t *voice_page{}, *camera_page{}, *face{}, *subtitle{}, *slider{}, *image{}, *info{};
lv_font_t* font{};
Handler handler{};
DeviceUiState voice_state{DeviceUiState::Idle};
bool camera_visible{false}, freetype_ready{false};
Image frame{};
lv_img_dsc_t descriptor{};
struct VoiceView {
  const char* title;
  const lv_img_dsc_t* face;
};
const VoiceView views[] = {{"说出唤醒词开始对话", &emoji_1f642_64},
                           {"正在聆听", &emoji_1f62f_64},
                           {"正在思考", &emoji_1f914_64},
                           {"正在回答，轻触可打断", &emoji_1f606_64},
                           {"回答完成", &emoji_1f642_64},
                           {"离线，等待网络恢复", &emoji_1f614_64},
                           {"发生错误，请重试", &emoji_1f614_64}};
void emit(Event event, std::uint8_t value = 0) {
  if (handler) {
    handler(event, value);
  }
}
lv_obj_t* label(lv_obj_t* parent, const char* text, int x, int y, int w, int h) {
  auto* object = lv_label_create(parent);
  lv_obj_set_pos(object, x, y);
  lv_obj_set_size(object, w, h);
  lv_obj_set_style_text_font(object, font, 0);
  lv_obj_set_style_text_align(object, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_text(object, text);
  return object;
}
void click(lv_event_t* event) {
  const auto* target = lv_event_get_target(event);
  if (target == face) {
    emit(voice_state == DeviceUiState::Speaking ? Event::Interrupt : Event::Wake);
  } else {
    camera(!camera_visible);
  }
}
void volume_changed(lv_event_t* event) {
  const auto code = lv_event_get_code(event);
  if (code == LV_EVENT_VALUE_CHANGED || code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
    const auto value = static_cast<std::uint8_t>(lv_slider_get_value(slider));
    emit(code == LV_EVENT_VALUE_CHANGED ? Event::Volume : Event::SaveVolume, value);
  }
}
bool has_glyph(std::uint32_t codepoint) {
  lv_font_glyph_dsc_t glyph{};
  return lv_font_get_glyph_dsc(font, &glyph, codepoint, 0) && glyph.resolved_font;
}
std::string supported_text(const char* text) {
  std::string result;
  // 仍保留缺失字形过滤，不能让云端emoji进入旧LVGL的缺字绘制路径。
  for (std::uint32_t at = 0; text[at];) {
    const auto start = at;
    const auto codepoint = _lv_txt_encoded_next(text, &at);
    if (codepoint == '\n' || (codepoint >= 32 && has_glyph(codepoint))) {
      result.append(text + start, at - start);
    }
  }
  return result;
}
lv_obj_t* container() {
  auto* object = lv_obj_create(lv_scr_act());
  lv_obj_set_size(object, 320, 240);
  lv_obj_set_style_pad_all(object, 0, 0);
  lv_obj_set_style_border_width(object, 0, 0);
  lv_obj_clear_flag(object, LV_OBJ_FLAG_SCROLLABLE);
  return object;
}
void button(lv_obj_t* parent, const char* text, int x) {
  auto* object = label(parent, text, x, 5, 85, 30);
  lv_obj_add_flag(object, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(object, click, LV_EVENT_CLICKED, nullptr);
}
}  // namespace

bool open(const char* font_path, Handler callback) {
  close();
  freetype_ready = lv_freetype_init(2, 4, 65536);
  lv_ft_info_t settings{};
  settings.name = font_path;
  settings.weight = 16;
  settings.style = FT_FONT_STYLE_NORMAL;
  if (!freetype_ready || !font_path || !lv_ft_font_init(&settings)) {
    close();
    return false;
  }
  font = settings.font;
  if (!has_glyph(0x667A)) {
    close();
    return false;
  }
  handler = callback;
  // 两页一次创建，切页只隐藏容器，不销毁后再依靠标志保护悬空控件。
  voice_page = container();
  label(voice_page, "boomPI", 5, 5, 110, 25);
  button(voice_page, "摄像头 >", 230);
  face = lv_img_create(voice_page);
  lv_obj_set_pos(face, 118, 37);
  lv_img_set_zoom(face, 328);
  lv_obj_add_flag(face, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(face, click, LV_EVENT_CLICKED, nullptr);
  subtitle = label(voice_page, "", 15, 126, 290, 80);
  lv_label_set_long_mode(subtitle, LV_LABEL_LONG_WRAP);
  label(voice_page, "音量", 5, 211, 45, 25);
  slider = lv_slider_create(voice_page);
  lv_obj_set_pos(slider, 65, 222);
  lv_obj_set_size(slider, 230, 7);
  lv_slider_set_range(slider, 0, 100);
  lv_obj_add_event_cb(slider, volume_changed, LV_EVENT_ALL, nullptr);
  camera_page = container();
  button(camera_page, "< 返回", 0);
  info = label(camera_page, "OFF", 160, 5, 150, 25);
  descriptor.header.cf = LV_IMG_CF_TRUE_COLOR;
  descriptor.header.w = kWidth;
  descriptor.header.h = kHeight;
  descriptor.data_size = frame.size() * sizeof(frame[0]);
  descriptor.data = reinterpret_cast<const std::uint8_t*>(frame.data());
  image = lv_img_create(camera_page);
  lv_img_set_src(image, &descriptor);
  lv_obj_set_pos(image, 0, 40);
  lv_obj_add_flag(camera_page, LV_OBJ_FLAG_HIDDEN);
  camera_visible = false;
  show(UiView{});
  present(CameraStatus::Stopped, false);
  return true;
}
void show(const UiView& view) {
  voice_state = view.state;
  const auto& voice = views[static_cast<unsigned>(view.state)];
  lv_img_set_src(face, voice.face);
  const auto text = supported_text(view.text.data());
  lv_label_set_text_fmt(subtitle, "%s\n%s", voice.title, text.c_str());
  lv_slider_set_value(slider, std::min<std::uint8_t>(view.volume, 100), LV_ANIM_OFF);
}
void camera(bool visible) {
  if (visible == camera_visible) {
    return;
  }
  camera_visible = visible;
  lv_obj_add_flag(visible ? voice_page : camera_page, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(visible ? camera_page : voice_page, LV_OBJ_FLAG_HIDDEN);
  present(visible ? CameraStatus::Starting : CameraStatus::Stopped, false);
  emit(visible ? Event::CameraOn : Event::CameraOff);
}
Image& pixels() noexcept {
  return frame;
}
void present(CameraStatus status, bool new_frame) {
  constexpr const char* labels[] = {"OFF", "START", "LIVE", "ERROR"};
  const char* text = labels[static_cast<unsigned>(status)];
  if (std::strcmp(lv_label_get_text(info), text) != 0) {
    lv_label_set_text(info, text);
  }
  if (status != CameraStatus::Live) {
    lv_obj_add_flag(image, LV_OBJ_FLAG_HIDDEN);
  } else if (new_frame) {
    lv_obj_clear_flag(image, LV_OBJ_FLAG_HIDDEN);
    lv_img_cache_invalidate_src(&descriptor);
    lv_obj_invalidate(image);
  }
}
void close() noexcept {
  // 页面先于字体释放；外部必须先停UI线程和摄像头生产者。
  if (voice_page) {
    lv_obj_del(voice_page);
    lv_obj_del(camera_page);
  }
  voice_page = camera_page = nullptr;
  if (font) {
    lv_ft_font_destroy(font);
    font = nullptr;
  }
  if (freetype_ready) {
    lv_freetype_destroy();
    freetype_ready = false;
  }
  handler = nullptr;
}
}  // namespace boompi::ui::page
