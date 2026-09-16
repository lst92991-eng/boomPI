#include "boompi/ui/lvgl_screen.h"

#include <ft2build.h>
#include <lvgl.h>
#include FT_FREETYPE_H

#include <algorithm>
#include <cstring>
#include <string>

#include "twemoji_64.h"

namespace boompi::ui::page {
namespace {
lv_obj_t *voice_page{}, *camera_page{}, *face{}, *status{}, *hint{}, *subtitle{};
lv_obj_t *talk{}, *talk_text{}, *slider{}, *volume_text{}, *image{}, *info{};
lv_font_t* font{};
Handler handler{};
DeviceUiState voice_state{DeviceUiState::Idle};
bool camera_visible{false};
bool shown{false};
Image frame{};
lv_img_dsc_t descriptor{};
struct VoiceView {
  const char* title;
  const char* hint;
  const lv_img_dsc_t* face;
};
const VoiceView views[] = {{"准备好了", "说出唤醒词或点开始", &emoji_1f642_64},
                           {"正在聆听", "请说，我在听", &emoji_1f62f_64},
                           {"正在思考", "可轻触停止等待", &emoji_1f914_64},
                           {"正在回答", "可以直接说话插话", &emoji_1f606_64},
                           {"连接已断开", "正在尝试重新连接", &emoji_1f614_64},
                           {"暂时遇到问题", "详情请查看终端日志", &emoji_1f614_64}};
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
  lv_obj_set_style_text_color(object, lv_color_hex(0x263B46), 0);
  lv_label_set_text(object, text);
  return object;
}
void click(lv_event_t* event) {
  const auto* target = lv_event_get_target(event);
  if (target == face || target == talk) {
    if (voice_state == DeviceUiState::Speaking || voice_state == DeviceUiState::Thinking) {
      emit(Event::Interrupt);
    } else if (voice_state == DeviceUiState::Idle) {
      emit(Event::Wake);
    }
  } else {
    camera(!camera_visible);
  }
}
void show_volume(std::uint8_t value) {
  if (value == 0) {
    lv_label_set_text(volume_text, "静音");
  } else {
    lv_label_set_text_fmt(volume_text, "音量  %u%%", value);
  }
}
void volume_changed(lv_event_t* event) {
  const auto code = lv_event_get_code(event);
  if (code == LV_EVENT_VALUE_CHANGED || code == LV_EVENT_RELEASED ||
      code == LV_EVENT_PRESS_LOST) {
    const auto value = static_cast<std::uint8_t>(lv_slider_get_value(slider));
    show_volume(value);
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
  lv_obj_set_style_radius(object, 0, 0);
  lv_obj_set_style_bg_color(object, lv_color_hex(0xF0F5F7), 0);
  lv_obj_clear_flag(object, LV_OBJ_FLAG_SCROLLABLE);
  return object;
}
lv_obj_t* button(lv_obj_t* parent, const char* text, int x, int y, int width, int height) {
  auto* object = lv_btn_create(parent);
  lv_obj_set_pos(object, x, y);
  lv_obj_set_size(object, width, height);
  lv_obj_set_style_bg_color(object, lv_color_hex(0x087F8C), 0);
  lv_obj_set_style_radius(object, 9, 0);
  lv_obj_set_style_shadow_width(object, 0, 0);
  auto* caption = label(object, text, 0, 0, width, 22);
  lv_obj_set_style_text_color(caption, lv_color_white(), 0);
  lv_obj_set_style_text_align(caption, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_center(caption);
  lv_obj_add_event_cb(object, click, LV_EVENT_CLICKED, nullptr);
  return object;
}
}  // namespace

bool open(const char* font_path, Handler callback) {
  close();
  // lv_init已创建进程级FreeType缓存，不能再次初始化覆盖旧句柄。
  // 旧版lv_ft_font_init失败会遗留名字引用，先用FreeType直接验证字体。
  FT_Library library{};
  FT_Face face_check{};
  if (!font_path || FT_Init_FreeType(&library) != 0) {
    return false;
  }
  const bool valid = FT_New_Face(library, font_path, 0, &face_check) == 0 &&
                     FT_Set_Pixel_Sizes(face_check, 0, 16) == 0 &&
                     FT_Get_Char_Index(face_check, 0x667A) != 0;
  if (face_check) {
    FT_Done_Face(face_check);
  }
  FT_Done_FreeType(library);
  lv_ft_info_t settings{};
  settings.name = font_path;
  settings.weight = 16;
  settings.style = FT_FONT_STYLE_NORMAL;
  if (!valid || !lv_ft_font_init(&settings)) {
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
  label(voice_page, "boomPI · 小智", 16, 12, 175, 24);
  button(voice_page, "摄像头", 228, 7, 80, 30);
  auto* card = lv_obj_create(voice_page);
  lv_obj_set_pos(card, 12, 43);
  lv_obj_set_size(card, 296, 139);
  lv_obj_set_style_radius(card, 12, 0);
  lv_obj_set_style_border_width(card, 0, 0);
  lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
  face = lv_img_create(voice_page);
  lv_obj_set_pos(face, 26, 49);
  lv_obj_add_flag(face, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(face, click, LV_EVENT_CLICKED, nullptr);
  status = label(voice_page, "", 109, 57, 184, 24);
  hint = label(voice_page, "", 109, 85, 184, 24);
  lv_obj_set_style_text_color(hint, lv_color_hex(0x617982), 0);
  subtitle = label(voice_page, "", 24, 119, 272, 59);
  lv_label_set_long_mode(subtitle, LV_LABEL_LONG_WRAP);
  talk = button(voice_page, "开始对话", 14, 193, 110, 36);
  talk_text = lv_obj_get_child(talk, 0);
  volume_text = label(voice_page, "", 145, 189, 165, 24);
  slider = lv_slider_create(voice_page);
  lv_obj_set_pos(slider, 152, 223);
  lv_obj_set_size(slider, 143, 7);
  lv_obj_set_ext_click_area(slider, 12);
  lv_obj_set_style_bg_color(slider, lv_color_hex(0x087F8C), LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(slider, lv_color_hex(0x087F8C), LV_PART_KNOB);
  lv_slider_set_range(slider, 0, 100);
  lv_obj_add_event_cb(slider, volume_changed, LV_EVENT_ALL, nullptr);
  camera_page = container();
  button(camera_page, "返回", 12, 7, 76, 30);
  label(camera_page, "摄像头预览", 112, 12, 190, 24);
  info = label(camera_page, "", 25, 98, 270, 60);
  lv_obj_set_style_text_align(info, LV_TEXT_ALIGN_CENTER, 0);
  descriptor.header.cf = LV_IMG_CF_TRUE_COLOR;
  descriptor.header.w = kWidth;
  descriptor.header.h = kHeight;
  descriptor.data_size = frame.size() * sizeof(frame[0]);
  descriptor.data = reinterpret_cast<const std::uint8_t*>(frame.data());
  image = lv_img_create(camera_page);
  lv_img_set_src(image, &descriptor);
  lv_obj_set_pos(image, 0, 46);
  lv_obj_add_flag(camera_page, LV_OBJ_FLAG_HIDDEN);
  camera_visible = false;
  shown = false;
  show(UiView{});
  present(CameraStatus::Stopped, false);
  return true;
}
void show(const UiView& view) {
  if (!shown || voice_state != view.state) {
    voice_state = view.state;
    const auto& voice = views[static_cast<unsigned>(view.state)];
    lv_img_set_src(face, voice.face);
    lv_label_set_text(status, voice.title);
    lv_label_set_text(hint, voice.hint);
    const bool busy =
        voice_state == DeviceUiState::Thinking || voice_state == DeviceUiState::Speaking;
    lv_label_set_text(talk_text, busy ? "停止" : "开始对话");
    if (busy || voice_state == DeviceUiState::Idle) {
      lv_obj_clear_state(talk, LV_STATE_DISABLED);
    } else {
      lv_obj_add_state(talk, LV_STATE_DISABLED);
    }
  }
  const auto text = supported_text(view.text.data());
  if (std::strcmp(lv_label_get_text(subtitle), text.c_str()) != 0) {
    lv_label_set_text(subtitle, text.c_str());
  }
  const auto level = std::min<std::uint8_t>(view.volume, 100);
  if (!shown ||
      (!lv_obj_has_state(slider, LV_STATE_PRESSED) && lv_slider_get_value(slider) != level)) {
    lv_slider_set_value(slider, level, LV_ANIM_OFF);
    show_volume(level);
  }
  shown = true;
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
  constexpr const char* labels[] = {"摄像头已关闭", "正在开启摄像头…", "",
                                    "暂时无法预览\n返回后可重新打开"};
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
  handler = nullptr;
}
}  // namespace boompi::ui::page
