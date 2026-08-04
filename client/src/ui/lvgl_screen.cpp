#include "boompi/ui/lvgl_screen.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>

#include "voice_orb_asset.h"

namespace boompi::ui {
namespace {

constexpr char kIconArrowLeft[] = "\xEE\xA8\x99";
constexpr char kIconCamera[] = "\xEE\xA9\x94";
constexpr char kIconClock[] = "\xEE\xA9\xB0";
constexpr char kIconCloud[] = "\xEE\xA9\xB6";
constexpr char kIconFolder[] = "\xEE\xAA\xAD";
constexpr char kIconMicrophone[] = "\xEE\xAB\xB0";
constexpr char kIconSun[] = "\xEE\xAC\xB0";
constexpr char kIconVolume[] = "\xEE\xAD\x91";
constexpr char kIconWifi[] = "\xEE\xAD\x92";
constexpr char kIconVolumeOff[] = "\xEF\x87\x83";
constexpr char kIconScanEye[] = "\xEF\x87\xBF";

struct AppInfo final {
  const char* name;
  const char* icon;
  const char* badge;
  const char* primary;
  const char* secondary;
  const char* action;
  std::array<const char*, 3> stat_titles;
  std::array<const char*, 3> stat_values;
  std::uint32_t accent;
};

const std::array<AppInfo, 7>& Apps() {
  static const std::array<AppInfo, 7> apps{{
      {"小智", kIconMicrophone, "语音在线", "随时可以说话", "低延迟全双工语音", "开始对话",
       {"唤醒", "麦克风", "状态"}, {"Snowboy", "双麦 AEC", "待命"}, 0x3478F6},
      {"摄像头", kIconCamera, "BSP 已验证", "SC3336", "3MP · MIPI CSI", "轻触启动预览",
       {"曝光", "帧率", "预览"}, {"自动", "4 FPS", "待启动"}, 0xC64F69},
      {"时间", kIconClock, "本地时间", "--:--", "使用系统本地时区", "时钟页面",
       {"闹钟", "计时器", "同步"}, {"待接入", "待接入", "系统时间"}, 0x5A687F},
      {"WiFi", kIconWifi, "配网入口", "boomPI-Setup", "首次联网 · 本地配置", "打开配网向导",
       {"热点", "方式", "状态"}, {"boomPI-Setup", "二维码", "按需启动"}, 0x2878C9},
      {"天气", kIconCloud, "数据待接入", "--°", "在线天气尚未接入", "后续接入",
       {"湿度", "风力", "降雨"}, {"--", "--", "--"}, 0x2C8799},
      {"资源", kIconFolder, "采集待接入", "RV1106", "系统监控尚未接入", "后续接入",
       {"内存", "NPU", "温度"}, {"--", "--", "--"}, 0x397C60},
      {"YOLO", kIconScanEye, "即将开放", "NPU 视觉", "桌面完成后再接入推理", "最后阶段接入",
       {"模型", "输入", "状态"}, {"--", "--", "待接入"}, 0x7658D7},
  }};
  return apps;
}

struct VoiceView final {
  const char* status;
  const char* hint;
  int amplitude;
  std::uint32_t period_ms;
};

const VoiceView& Voice(DeviceUiState state) {
  // 条目顺序必须与 DeviceUiState 一致；新状态追加在枚举末尾。
  static const std::array<VoiceView, 8> views{{
      {"随时可以说话", "说出唤醒词开始对话", 2, 2200},
      {"正在聆听", "我在听，请继续", 8, 760},
      {"正在思考", "正在组织回答", 5, 1200},
      {"正在回答", "轻触屏幕即可打断", 10, 620},
      {"回答完成", "随时可以继续问我", 3, 1800},
      {"暂时离线", "等待网络恢复", 0, 2400},
      {"发生错误", "请检查网络后重试", 1, 2000},
      {"即将说完", "轻触屏幕仍可打断", 4, 1500},
  }};
  return views[static_cast<std::size_t>(state)];
}

void ConfigurePage(lv_obj_t* page, std::uint32_t top, std::uint32_t bottom) {
  lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_size(page, 320, 240);
  lv_obj_set_pos(page, 0, 0);
  lv_obj_set_style_radius(page, 0, 0);
  lv_obj_set_style_border_width(page, 0, 0);
  lv_obj_set_style_pad_all(page, 0, 0);
  lv_obj_set_style_bg_color(page, lv_color_hex(top), 0);
  lv_obj_set_style_bg_grad_color(page, lv_color_hex(bottom), 0);
  lv_obj_set_style_bg_grad_dir(page, LV_GRAD_DIR_VER, 0);
  lv_obj_set_style_bg_opa(page, LV_OPA_COVER, 0);
}

void StyleLabel(lv_obj_t* label, const lv_font_t* font, std::uint32_t color,
                lv_text_align_t alignment = LV_TEXT_ALIGN_CENTER) {
  lv_obj_set_style_text_font(label, font, 0);
  lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
  lv_obj_set_style_text_align(label, alignment, 0);
  lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
}

void ConfigureCircle(lv_obj_t* object, int size, std::uint32_t color,
                     lv_opa_t opacity = LV_OPA_COVER) {
  lv_obj_clear_flag(object, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_size(object, size, size);
  lv_obj_set_style_radius(object, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_border_width(object, 0, 0);
  lv_obj_set_style_pad_all(object, 0, 0);
  lv_obj_set_style_bg_color(object, lv_color_hex(color), 0);
  lv_obj_set_style_bg_opa(object, opacity, 0);
}

bool HasGlyph(const lv_font_t* font, std::uint32_t codepoint) {
  lv_font_glyph_dsc_t glyph{};
  return lv_font_get_glyph_dsc(font, &glyph, codepoint, 0U) &&
         glyph.resolved_font != nullptr;
}

bool DecodeUtf8(std::string_view text, std::size_t* offset,
                std::uint32_t* codepoint) {
  const std::size_t start = *offset;
  const auto lead = static_cast<unsigned char>(text[start]);
  std::size_t bytes = 1U;
  std::uint32_t value = lead;
  if ((lead & 0xE0U) == 0xC0U) {
    bytes = 2U;
    value = lead & 0x1FU;
  } else if ((lead & 0xF0U) == 0xE0U) {
    bytes = 3U;
    value = lead & 0x0FU;
  } else if ((lead & 0xF8U) == 0xF0U) {
    bytes = 4U;
    value = lead & 0x07U;
  } else if (lead >= 0x80U) {
    ++*offset;
    return false;
  }
  if (start + bytes > text.size()) {
    *offset = text.size();
    return false;
  }
  for (std::size_t index = 1U; index < bytes; ++index) {
    const auto next = static_cast<unsigned char>(text[start + index]);
    if ((next & 0xC0U) != 0x80U) {
      ++*offset;
      return false;
    }
    value = (value << 6U) | (next & 0x3FU);
  }
  const bool overlong = (bytes == 2U && value < 0x80U) ||
                        (bytes == 3U && value < 0x800U) ||
                        (bytes == 4U && value < 0x10000U);
  if (overlong || value > 0x10FFFFU ||
      (value >= 0xD800U && value <= 0xDFFFU)) {
    ++*offset;
    return false;
  }
  *offset = start + bytes;
  *codepoint = value;
  return true;
}

std::string SupportedText(const lv_font_t* font, std::string_view text) {
  std::string supported;
  supported.reserve(text.size());
  for (std::size_t offset = 0U; offset < text.size();) {
    const std::size_t start = offset;
    std::uint32_t codepoint = 0U;
    if (!DecodeUtf8(text, &offset, &codepoint)) continue;
    if (codepoint == static_cast<std::uint32_t>('\n') ||
        (codepoint >= 0x20U && HasGlyph(font, codepoint))) {
      supported.append(text.substr(start, offset - start));
    }
  }
  return supported;
}

void SetLabelIfChanged(lv_obj_t* label, const char* text) {
  if (std::strcmp(lv_label_get_text(label), text) != 0) lv_label_set_text(label, text);
}

lv_obj_t* MakeBackButton(lv_obj_t* parent, const lv_font_t* icon_font,
                         lv_event_cb_t callback, void* user_data) {
  lv_obj_t* button = lv_obj_create(parent);
  lv_obj_clear_flag(button, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_size(button, 44, 44);
  lv_obj_set_pos(button, 0, 0);
  lv_obj_set_style_border_width(button, 0, 0);
  lv_obj_set_style_bg_opa(button, LV_OPA_TRANSP, 0);
  lv_obj_set_style_pad_all(button, 0, 0);
  lv_obj_set_style_translate_y(button, 1, LV_STATE_PRESSED);
  lv_obj_add_flag(button, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, user_data);
  lv_obj_t* icon = lv_label_create(button);
  lv_label_set_text(icon, kIconArrowLeft);
  StyleLabel(icon, icon_font, 0xF4F7FC);
  lv_obj_center(icon);
  return button;
}

}  // namespace

bool LvglScreen::Create(const char* font_path, const char* icon_font_path) noexcept {
  lv_obj_t* root = lv_scr_act();
  lv_obj_clean(root);
  lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_color(root, lv_color_hex(0x070B14), 0);
  lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);

  if (font_path == nullptr || icon_font_path == nullptr ||
      !lv_freetype_init(8, 8, 98304)) return false;
  freetype_initialized_ = true;
  auto load_font = [font_path](std::uint16_t weight, lv_font_t** font) {
    lv_ft_info_t info{};
    info.name = font_path;
    info.weight = weight;
    info.style = FT_FONT_STYLE_NORMAL;
    if (lv_ft_font_init(&info)) *font = info.font;
  };
  load_font(10, &home_caption_font_);
  load_font(12, &desktop_font_);
  load_font(14, &small_font_);
  load_font(17, &body_font_);
  load_font(24, &title_font_);
  load_font(34, &clock_font_);
  auto load_icon_font = [icon_font_path](std::uint16_t weight, lv_font_t** font) {
    lv_ft_info_t info{};
    info.name = icon_font_path;
    info.weight = weight;
    info.style = FT_FONT_STYLE_NORMAL;
    if (lv_ft_font_init(&info)) *font = info.font;
  };
  load_icon_font(34, &icon_font_);
  load_icon_font(20, &icon_small_font_);
  if (home_caption_font_ == nullptr || desktop_font_ == nullptr || small_font_ == nullptr ||
      body_font_ == nullptr || title_font_ == nullptr ||
      clock_font_ == nullptr || icon_font_ == nullptr || icon_small_font_ == nullptr ||
      !HasGlyph(small_font_, 0x667AU) ||
      !HasGlyph(clock_font_, static_cast<std::uint32_t>('0')) ||
      !HasGlyph(icon_font_, 0xEA54U)) {
    Destroy();
    return false;
  }

  BuildHome(root);
  BuildVoice(root);
  BuildCamera(root);
  BuildWifiProvision(root);
  BuildGeneric(root);
  clock_timer_ = lv_timer_create(ClockTick, 1000, this);
  UpdateClock();
  ShowHome();
  return true;
}

void LvglScreen::BuildHome(lv_obj_t* root) noexcept {
  home_ = lv_obj_create(root);
  ConfigurePage(home_, 0x0D1522, 0x0D1422);
  lv_obj_set_style_bg_grad_dir(home_, LV_GRAD_DIR_HOR, 0);

  lv_obj_t* ambient = lv_obj_create(home_);
  lv_obj_clear_flag(ambient, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_clear_flag(ambient, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_size(ambient, 194, 194);
  lv_obj_set_pos(ambient, -54, 20);
  lv_obj_set_style_radius(ambient, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_border_width(ambient, 0, 0);
  lv_obj_set_style_bg_color(ambient, lv_color_hex(0x17243A), 0);
  lv_obj_set_style_bg_opa(ambient, LV_OPA_20, 0);
  lv_obj_set_style_pad_all(ambient, 0, 0);

  auto make_orbit = [this](int size, int x, int y, std::uint32_t color,
                           lv_opa_t opacity) {
    lv_obj_t* orbit = lv_obj_create(home_);
    lv_obj_clear_flag(orbit, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(orbit, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(orbit, size, size);
    lv_obj_set_pos(orbit, x, y);
    lv_obj_set_style_radius(orbit, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(orbit, 1, 0);
    lv_obj_set_style_border_color(orbit, lv_color_hex(color), 0);
    lv_obj_set_style_border_opa(orbit, opacity, 0);
    lv_obj_set_style_bg_opa(orbit, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(orbit, 0, 0);
  };
  make_orbit(214, -64, 10, 0x34445E, LV_OPA_40);
  make_orbit(194, -54, 20, 0x3475C7, LV_OPA_50);

  static lv_point_t divider_points[]{{167, 0}, {159, 38}, {154, 82},
                                      {153, 126}, {148, 183}, {130, 239}};
  lv_obj_t* divider_curve = lv_line_create(home_);
  lv_line_set_points(divider_curve, divider_points,
                     sizeof(divider_points) / sizeof(divider_points[0]));
  lv_obj_clear_flag(divider_curve, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_clear_flag(divider_curve, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_line_width(divider_curve, 1, 0);
  lv_obj_set_style_line_color(divider_curve, lv_color_hex(0x344052), 0);
  lv_obj_set_style_line_opa(divider_curve, LV_OPA_40, 0);

  lv_obj_t* orbit_dot = lv_obj_create(home_);
  ConfigureCircle(orbit_dot, 7, 0x4DA6FF, LV_OPA_COVER);
  lv_obj_clear_flag(orbit_dot, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_shadow_width(orbit_dot, 6, 0);
  lv_obj_set_style_shadow_color(orbit_dot, lv_color_hex(0x4DA6FF), 0);
  lv_obj_set_style_shadow_opa(orbit_dot, LV_OPA_40, 0);
  lv_obj_set_pos(orbit_dot, 136, 114);

  lv_obj_t* sun = lv_label_create(home_);
  lv_label_set_text(sun, kIconSun);
  StyleLabel(sun, icon_small_font_, 0xF3EEE6, LV_TEXT_ALIGN_LEFT);
  lv_obj_set_size(sun, 24, 23);
  lv_obj_set_pos(sun, 16, 39);

  lv_obj_t* temperature = lv_label_create(home_);
  lv_label_set_text(temperature, "--°");
  StyleLabel(temperature, body_font_, 0xF3EEE6, LV_TEXT_ALIGN_LEFT);
  lv_obj_set_size(temperature, 72, 24);
  lv_obj_set_pos(temperature, 43, 42);

  lv_obj_t* weather = lv_label_create(home_);
  lv_label_set_text(weather, "天气待接入");
  StyleLabel(weather, home_caption_font_, 0xA5B0C0, LV_TEXT_ALIGN_LEFT);
  lv_obj_set_size(weather, 120, 20);
  lv_obj_set_pos(weather, 42, 64);

  home_clock_ = lv_label_create(home_);
  StyleLabel(home_clock_, clock_font_, 0xF3EEE6, LV_TEXT_ALIGN_LEFT);
  lv_obj_set_size(home_clock_, 144, 43);
  lv_obj_set_pos(home_clock_, 17, 79);

  home_date_ = lv_label_create(home_);
  StyleLabel(home_date_, desktop_font_, 0xD8D5D0, LV_TEXT_ALIGN_LEFT);
  lv_obj_set_size(home_date_, 145, 18);
  lv_obj_set_pos(home_date_, 19, 120);

  home_year_ = lv_label_create(home_);
  StyleLabel(home_year_, home_caption_font_, 0x78869A, LV_TEXT_ALIGN_LEFT);
  lv_obj_set_size(home_year_, 100, 17);
  lv_obj_set_pos(home_year_, 19, 136);

  lv_obj_t* voice_cell = lv_obj_create(home_);
  lv_obj_clear_flag(voice_cell, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(voice_cell, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_size(voice_cell, 152, 70);
  lv_obj_set_pos(voice_cell, 0, 154);
  lv_obj_set_style_border_width(voice_cell, 0, 0);
  lv_obj_set_style_bg_opa(voice_cell, LV_OPA_TRANSP, 0);
  lv_obj_set_style_pad_all(voice_cell, 0, 0);
  lv_obj_set_style_translate_y(voice_cell, 1, LV_STATE_PRESSED);
  app_buttons_[0] = voice_cell;
  lv_obj_add_event_cb(voice_cell, AppClicked, LV_EVENT_CLICKED, this);

  lv_obj_t* mini_orb = lv_obj_create(voice_cell);
  lv_obj_clear_flag(mini_orb, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_clear_flag(mini_orb, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_size(mini_orb, 58, 58);
  lv_obj_set_pos(mini_orb, 13, 3);
  lv_obj_set_style_border_width(mini_orb, 0, 0);
  lv_obj_set_style_bg_opa(mini_orb, LV_OPA_TRANSP, 0);
  lv_obj_set_style_pad_all(mini_orb, 0, 0);
  home_orb_image_ = lv_img_create(mini_orb);
  lv_img_set_src(home_orb_image_, assets::VoiceOrb());
  lv_img_set_pivot(home_orb_image_, 56, 56);
  lv_img_set_antialias(home_orb_image_, true);
  lv_img_set_zoom(home_orb_image_, 116);
  lv_obj_clear_flag(home_orb_image_, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_center(home_orb_image_);

  lv_obj_t* voice_name = lv_label_create(voice_cell);
  lv_label_set_text(voice_name, "小智");
  StyleLabel(voice_name, home_caption_font_, 0xF3EEE6, LV_TEXT_ALIGN_LEFT);
  lv_obj_set_size(voice_name, 72, 16);
  lv_obj_set_pos(voice_name, 70, 16);
  lv_obj_t* voice_hint = lv_label_create(voice_cell);
  lv_label_set_text(voice_hint, "点我说话");
  StyleLabel(voice_hint, home_caption_font_, 0x738199, LV_TEXT_ALIGN_LEFT);
  lv_obj_set_size(voice_hint, 78, 14);
  lv_obj_set_pos(voice_hint, 70, 31);

  for (int i = 0; i < 2; ++i) {
    lv_obj_t* page_dot = lv_obj_create(home_);
    ConfigureCircle(page_dot, i == 0 ? 5 : 4,
                    i == 0 ? 0x53657E : 0x39475A, LV_OPA_70);
    lv_obj_clear_flag(page_dot, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(page_dot, 86 + i * 11, 217);
  }

  const auto& apps = Apps();
  for (std::size_t i = 1; i < apps.size(); ++i) {
    const int slot = static_cast<int>(i - 1U);
    const int column = slot % 2;
    const int row = slot / 2;
    lv_obj_t* cell = lv_obj_create(home_);
    lv_obj_clear_flag(cell, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(cell, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(cell, 64, 58);
    lv_obj_set_pos(cell, 178 + column * 64, 32 + row * 61);
    lv_obj_set_style_border_width(cell, 0, 0);
    lv_obj_set_style_bg_opa(cell, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_color(cell, lv_color_hex(0x1A293C), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(cell, LV_OPA_40, LV_STATE_PRESSED);
    lv_obj_set_style_radius(cell, 18, LV_STATE_PRESSED);
    lv_obj_set_style_pad_all(cell, 0, 0);
    app_buttons_[i] = cell;
    lv_obj_add_event_cb(cell, AppClicked, LV_EVENT_CLICKED, this);

    lv_obj_t* icon = lv_label_create(cell);
    lv_label_set_text(icon, apps[i].icon);
    const std::uint32_t icon_color = i == 3U ? 0x4DA6FF :
                                     (i == 6U ? 0x667389 : 0xF3EEE6);
    StyleLabel(icon, icon_font_, icon_color);
    lv_obj_set_size(icon, 48, 36);
    lv_obj_align(icon, LV_ALIGN_TOP_MID, 0, 0);

    if (i == 1U) {
      lv_obj_t* accent = lv_obj_create(cell);
      ConfigureCircle(accent, 4, 0x4DA6FF, LV_OPA_COVER);
      lv_obj_clear_flag(accent, LV_OBJ_FLAG_CLICKABLE);
      lv_obj_set_pos(accent, 42, 7);
    } else if (i == 4U) {
      lv_obj_t* accent = lv_label_create(cell);
      lv_label_set_text(accent, kIconSun);
      StyleLabel(accent, icon_small_font_, 0x4DA6FF);
      lv_obj_set_size(accent, 23, 23);
      lv_obj_set_pos(accent, 38, -1);
    }

    lv_obj_t* name = lv_label_create(cell);
    lv_label_set_text(name, apps[i].name);
    StyleLabel(name, home_caption_font_, i == 6U ? 0x707A8B : 0xF3EEE6);
    lv_obj_set_size(name, 64, 14);
    lv_obj_set_pos(name, 0, row == 2 ? 34 : 38);
    if (i == 6U) {
      lv_obj_t* hint = lv_label_create(cell);
      lv_label_set_text(hint, "即将开放");
      StyleLabel(hint, home_caption_font_, 0x657084);
      lv_obj_set_size(hint, 64, 12);
      lv_obj_set_pos(hint, 0, 45);
    }
  }
}

void LvglScreen::BuildVoice(lv_obj_t* root) noexcept {
  voice_ = lv_obj_create(root);
  ConfigurePage(voice_, 0x0D1522, 0x0D1422);
  lv_obj_set_style_bg_grad_dir(voice_, LV_GRAD_DIR_HOR, 0);
  MakeBackButton(voice_, icon_small_font_, BackClicked, this);

  lv_obj_t* title = lv_label_create(voice_);
  lv_label_set_text(title, "小智");
  StyleLabel(title, body_font_, 0xF3EEE6);
  lv_obj_set_size(title, 120, 23);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);

  lv_obj_t* mute = lv_obj_create(voice_);
  lv_obj_clear_flag(mute, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_size(mute, 44, 44);
  lv_obj_align(mute, LV_ALIGN_TOP_RIGHT, 0, 0);
  lv_obj_set_style_border_width(mute, 0, 0);
  lv_obj_set_style_bg_opa(mute, LV_OPA_TRANSP, 0);
  lv_obj_set_style_pad_all(mute, 0, 0);
  lv_obj_add_flag(mute, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(mute, MuteClicked, LV_EVENT_CLICKED, this);
  voice_mute_icon_ = lv_label_create(mute);
  lv_label_set_text(voice_mute_icon_, kIconVolume);
  StyleLabel(voice_mute_icon_, icon_small_font_, 0xF3EEE6);
  lv_obj_center(voice_mute_icon_);

  for (int i = 0; i < 2; ++i) {
    voice_orbits_[i] = lv_obj_create(voice_);
    lv_obj_clear_flag(voice_orbits_[i], LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(voice_orbits_[i], LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(voice_orbits_[i], 142 + i * 28, 142 + i * 28);
    lv_obj_align(voice_orbits_[i], LV_ALIGN_TOP_MID, 0, 27 - i * 14);
    lv_obj_set_style_radius(voice_orbits_[i], LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(voice_orbits_[i], 1, 0);
    lv_obj_set_style_border_color(voice_orbits_[i],
                                  lv_color_hex(i == 0 ? 0x315F9D : 0x34445E), 0);
    lv_obj_set_style_border_opa(voice_orbits_[i], i == 0 ? LV_OPA_40 : LV_OPA_20, 0);
    lv_obj_set_style_bg_color(voice_orbits_[i], lv_color_hex(0x17243A), 0);
    lv_obj_set_style_bg_opa(voice_orbits_[i], i == 0 ? LV_OPA_10 : LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(voice_orbits_[i], 0, 0);
  }

  lv_obj_t* orb = lv_obj_create(voice_);
  lv_obj_clear_flag(orb, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(orb, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
  lv_obj_add_flag(orb, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(orb, VoiceClicked, LV_EVENT_CLICKED, this);
  lv_obj_set_size(orb, 138, 124);
  lv_obj_align(orb, LV_ALIGN_TOP_MID, 0, 35);
  lv_obj_set_style_border_width(orb, 0, 0);
  lv_obj_set_style_bg_opa(orb, LV_OPA_TRANSP, 0);
  lv_obj_set_style_pad_all(orb, 0, 0);

  voice_orb_image_ = lv_img_create(orb);
  lv_img_set_src(voice_orb_image_, assets::VoiceOrb());
  lv_img_set_pivot(voice_orb_image_, 56, 56);
  lv_img_set_antialias(voice_orb_image_, true);
  lv_img_set_zoom(voice_orb_image_, LV_IMG_ZOOM_NONE);
  lv_obj_clear_flag(voice_orb_image_, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_center(voice_orb_image_);

  voice_status_ = lv_label_create(voice_);
  StyleLabel(voice_status_, body_font_, 0xF3EEE6);
  lv_obj_set_size(voice_status_, 280, 24);
  lv_obj_align(voice_status_, LV_ALIGN_TOP_MID, 0, 160);

  voice_text_ = lv_label_create(voice_);
  StyleLabel(voice_text_, small_font_, 0x8794A7);
  lv_label_set_long_mode(voice_text_, LV_LABEL_LONG_WRAP);
  lv_obj_set_size(voice_text_, 286, 27);
  lv_obj_align(voice_text_, LV_ALIGN_BOTTOM_MID, 0, -25);

  voice_volume_slider_ = lv_slider_create(voice_);
  lv_slider_set_range(voice_volume_slider_, 0, 100);
  lv_obj_set_size(voice_volume_slider_, 214, 8);
  lv_obj_align(voice_volume_slider_, LV_ALIGN_BOTTOM_LEFT, 35, -8);
  lv_obj_set_style_radius(voice_volume_slider_, LV_RADIUS_CIRCLE,
                          LV_PART_MAIN);
  lv_obj_set_style_bg_color(voice_volume_slider_, lv_color_hex(0x263348),
                            LV_PART_MAIN);
  lv_obj_set_style_bg_opa(voice_volume_slider_, LV_OPA_COVER,
                          LV_PART_MAIN);
  lv_obj_set_style_bg_color(voice_volume_slider_, lv_color_hex(0x4DA6FF),
                            LV_PART_INDICATOR);
  lv_obj_set_style_bg_opa(voice_volume_slider_, LV_OPA_COVER,
                          LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(voice_volume_slider_, lv_color_hex(0xF3EEE6),
                            LV_PART_KNOB);
  lv_obj_set_style_pad_all(voice_volume_slider_, 4, LV_PART_KNOB);
  lv_obj_add_event_cb(voice_volume_slider_, VolumeChanged, LV_EVENT_PRESSED,
                      this);
  lv_obj_add_event_cb(voice_volume_slider_, VolumeChanged,
                      LV_EVENT_VALUE_CHANGED, this);
  lv_obj_add_event_cb(voice_volume_slider_, VolumeChanged, LV_EVENT_RELEASED,
                      this);
  lv_obj_add_event_cb(voice_volume_slider_, VolumeChanged, LV_EVENT_PRESS_LOST,
                      this);

  voice_volume_value_ = lv_label_create(voice_);
  StyleLabel(voice_volume_value_, home_caption_font_, 0xAAB6C7,
             LV_TEXT_ALIGN_RIGHT);
  lv_obj_set_size(voice_volume_value_, 48, 16);
  lv_obj_align(voice_volume_value_, LV_ALIGN_BOTTOM_RIGHT, -9, -3);
  SetVolume(volume_percent_);
}

void LvglScreen::BuildCamera(lv_obj_t* root) noexcept {
  camera_ = lv_obj_create(root);
  ConfigurePage(camera_, 0x05070B, 0x05070B);

  camera_descriptor_ = {};
  camera_descriptor_.header.cf = LV_IMG_CF_TRUE_COLOR;
  camera_descriptor_.header.w = static_cast<std::uint32_t>(kCameraWidth);
  camera_descriptor_.header.h = static_cast<std::uint32_t>(kCameraHeight);
  camera_descriptor_.data_size = camera_pixels_.size() * sizeof(camera_pixels_[0]);
  camera_descriptor_.data = reinterpret_cast<const std::uint8_t*>(
      camera_pixels_.data());

  camera_image_ = lv_img_create(camera_);
  lv_img_set_src(camera_image_, &camera_descriptor_);
  lv_obj_set_pos(camera_image_, 0, 30);
  lv_obj_clear_flag(camera_image_, LV_OBJ_FLAG_CLICKABLE);

  lv_obj_t* title = lv_label_create(camera_);
  lv_label_set_text(title, "SC3336");
  StyleLabel(title, small_font_, 0xF3EEE6);
  lv_obj_set_size(title, 120, 20);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 7);

  camera_status_ = lv_label_create(camera_);
  lv_label_set_text(camera_status_, "STARTING");
  StyleLabel(camera_status_, home_caption_font_, 0x4DA6FF,
             LV_TEXT_ALIGN_RIGHT);
  lv_obj_set_size(camera_status_, 78, 16);
  lv_obj_align(camera_status_, LV_ALIGN_TOP_RIGHT, -10, 9);

  lv_obj_t* footer = lv_label_create(camera_);
  lv_label_set_text(footer, "320 x 180  |  LIVE");
  StyleLabel(footer, home_caption_font_, 0x8A96A8);
  lv_obj_set_size(footer, 180, 16);
  lv_obj_align(footer, LV_ALIGN_BOTTOM_MID, 0, -7);

  // Create navigation last so it remains clickable above the video image.
  MakeBackButton(camera_, icon_small_font_, BackClicked, this);
}

void LvglScreen::BuildWifiProvision(lv_obj_t* root) noexcept {
  wifi_provision_ = lv_obj_create(root);
  ConfigurePage(wifi_provision_, 0x0D1522, 0x0D1422);
  lv_obj_set_style_bg_grad_dir(wifi_provision_, LV_GRAD_DIR_HOR, 0);
  MakeBackButton(wifi_provision_, icon_small_font_, BackClicked, this);

  lv_obj_t* title = lv_label_create(wifi_provision_);
  lv_label_set_text(title, "WiFi 配网");
  StyleLabel(title, body_font_, 0xF3EEE6);
  lv_obj_set_size(title, 150, 23);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);

  lv_obj_t* badge = lv_label_create(wifi_provision_);
  lv_label_set_text(badge, "等待扫码");
  StyleLabel(badge, home_caption_font_, 0x4DA6FF, LV_TEXT_ALIGN_RIGHT);
  lv_obj_set_size(badge, 76, 18);
  lv_obj_align(badge, LV_ALIGN_TOP_RIGHT, -12, 13);

  lv_obj_t* orbit = lv_obj_create(wifi_provision_);
  lv_obj_clear_flag(orbit, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_clear_flag(orbit, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_size(orbit, 190, 190);
  lv_obj_set_pos(orbit, -38, 36);
  lv_obj_set_style_radius(orbit, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_border_width(orbit, 1, 0);
  lv_obj_set_style_border_color(orbit, lv_color_hex(0x315F9D), 0);
  lv_obj_set_style_border_opa(orbit, LV_OPA_40, 0);
  lv_obj_set_style_bg_opa(orbit, LV_OPA_TRANSP, 0);

  lv_obj_t* qr_card = lv_obj_create(wifi_provision_);
  lv_obj_clear_flag(qr_card, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_size(qr_card, 116, 116);
  lv_obj_set_pos(qr_card, 18, 54);
  lv_obj_set_style_radius(qr_card, 10, 0);
  lv_obj_set_style_border_width(qr_card, 0, 0);
  lv_obj_set_style_bg_color(qr_card, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_bg_opa(qr_card, LV_OPA_COVER, 0);
  lv_obj_set_style_pad_all(qr_card, 0, 0);

  lv_obj_t* qr = lv_qrcode_create(qr_card, 104, lv_color_hex(0x08111C),
                                  lv_color_hex(0xFFFFFF));
  lv_obj_center(qr);
  static constexpr char kProvisioningQr[] =
      "WIFI:T:WPA;S:boomPI-Setup;P:boompi-setup;;";
  lv_qrcode_update(qr, kProvisioningQr, sizeof(kProvisioningQr) - 1U);

  lv_obj_t* hotspot = lv_label_create(wifi_provision_);
  lv_label_set_text(hotspot, "boomPI-Setup");
  StyleLabel(hotspot, small_font_, 0xF3EEE6);
  lv_obj_set_size(hotspot, 126, 20);
  lv_obj_set_pos(hotspot, 13, 177);

  lv_obj_t* scan_hint = lv_label_create(wifi_provision_);
  lv_label_set_text(scan_hint, "手机扫码连接设备");
  StyleLabel(scan_hint, home_caption_font_, 0x7D8BA0);
  lv_obj_set_size(scan_hint, 132, 16);
  lv_obj_set_pos(scan_hint, 10, 200);

  lv_obj_t* divider = lv_obj_create(wifi_provision_);
  lv_obj_clear_flag(divider, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_clear_flag(divider, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_size(divider, 1, 174);
  lv_obj_set_pos(divider, 154, 50);
  lv_obj_set_style_border_width(divider, 0, 0);
  lv_obj_set_style_bg_color(divider, lv_color_hex(0x2B3A4F), 0);
  lv_obj_set_style_bg_opa(divider, LV_OPA_60, 0);

  lv_obj_t* guide_title = lv_label_create(wifi_provision_);
  lv_label_set_text(guide_title, "三步完成配网");
  StyleLabel(guide_title, small_font_, 0xF3EEE6, LV_TEXT_ALIGN_LEFT);
  lv_obj_set_size(guide_title, 146, 20);
  lv_obj_set_pos(guide_title, 168, 53);

  constexpr std::array<const char*, 3> step_titles{{
      "扫码连接", "打开页面", "选择网络"}};
  constexpr std::array<const char*, 3> step_details{{
      "连接 boomPI-Setup", "自动进入配网页", "保存后设备重启"}};
  for (std::size_t i = 0; i < step_titles.size(); ++i) {
    const int y = 82 + static_cast<int>(i) * 45;
    lv_obj_t* number = lv_label_create(wifi_provision_);
    char number_text[2]{static_cast<char>('1' + i), '\0'};
    lv_label_set_text(number, number_text);
    StyleLabel(number, small_font_, 0x4DA6FF, LV_TEXT_ALIGN_LEFT);
    lv_obj_set_size(number, 18, 20);
    lv_obj_set_pos(number, 168, y);

    lv_obj_t* step = lv_label_create(wifi_provision_);
    lv_label_set_text(step, step_titles[i]);
    StyleLabel(step, desktop_font_, 0xF3EEE6, LV_TEXT_ALIGN_LEFT);
    lv_obj_set_size(step, 118, 18);
    lv_obj_set_pos(step, 190, y);

    lv_obj_t* detail = lv_label_create(wifi_provision_);
    lv_label_set_text(detail, step_details[i]);
    StyleLabel(detail, home_caption_font_, 0x77859A, LV_TEXT_ALIGN_LEFT);
    lv_obj_set_size(detail, 122, 15);
    lv_obj_set_pos(detail, 190, y + 20);
  }
}

void LvglScreen::BuildGeneric(lv_obj_t* root) noexcept {
  generic_ = lv_obj_create(root);
  ConfigurePage(generic_, 0x0D1522, 0x0D1422);
  lv_obj_set_style_bg_grad_dir(generic_, LV_GRAD_DIR_HOR, 0);
  MakeBackButton(generic_, icon_small_font_, BackClicked, this);

  generic_title_ = lv_label_create(generic_);
  StyleLabel(generic_title_, body_font_, 0xF3EEE6);
  lv_obj_set_size(generic_title_, 150, 23);
  lv_obj_align(generic_title_, LV_ALIGN_TOP_MID, 0, 10);

  generic_badge_ = lv_label_create(generic_);
  StyleLabel(generic_badge_, desktop_font_, 0x77859A, LV_TEXT_ALIGN_RIGHT);
  lv_obj_set_size(generic_badge_, 112, 19);
  lv_obj_align(generic_badge_, LV_ALIGN_TOP_RIGHT, -10, 12);

  for (int i = 0; i < 2; ++i) {
    lv_obj_t* orbit = lv_obj_create(generic_);
    lv_obj_clear_flag(orbit, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(orbit, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(orbit, 190 + i * 28, 190 + i * 28);
    lv_obj_set_pos(orbit, -58 - i * 14, 43 - i * 14);
    lv_obj_set_style_radius(orbit, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(orbit, 1, 0);
    lv_obj_set_style_border_color(orbit, lv_color_hex(i == 0 ? 0x315F9D : 0x34445E), 0);
    lv_obj_set_style_border_opa(orbit, i == 0 ? LV_OPA_50 : LV_OPA_30, 0);
    lv_obj_set_style_bg_color(orbit, lv_color_hex(0x17243A), 0);
    lv_obj_set_style_bg_opa(orbit, i == 0 ? LV_OPA_20 : LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(orbit, 0, 0);
  }

  generic_hero_ = lv_obj_create(generic_);
  lv_obj_clear_flag(generic_hero_, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(generic_hero_, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(generic_hero_, AppActionClicked, LV_EVENT_CLICKED, this);
  lv_obj_set_size(generic_hero_, 158, 188);
  lv_obj_set_pos(generic_hero_, 0, 44);
  lv_obj_set_style_border_width(generic_hero_, 0, 0);
  lv_obj_set_style_bg_opa(generic_hero_, LV_OPA_TRANSP, 0);
  lv_obj_set_style_pad_all(generic_hero_, 0, 0);

  generic_icon_ = lv_label_create(generic_hero_);
  StyleLabel(generic_icon_, icon_font_, 0xF3EEE6);
  lv_obj_set_size(generic_icon_, 64, 42);
  lv_obj_set_pos(generic_icon_, 20, 18);

  generic_primary_ = lv_label_create(generic_hero_);
  StyleLabel(generic_primary_, title_font_, 0xF3EEE6, LV_TEXT_ALIGN_LEFT);
  lv_obj_set_size(generic_primary_, 142, 34);
  lv_obj_set_pos(generic_primary_, 18, 68);

  generic_secondary_ = lv_label_create(generic_hero_);
  StyleLabel(generic_secondary_, desktop_font_, 0x8B98AA, LV_TEXT_ALIGN_LEFT);
  lv_label_set_long_mode(generic_secondary_, LV_LABEL_LONG_WRAP);
  lv_obj_set_size(generic_secondary_, 132, 42);
  lv_obj_set_pos(generic_secondary_, 18, 105);

  generic_action_ = lv_label_create(generic_hero_);
  StyleLabel(generic_action_, desktop_font_, 0x4DA6FF, LV_TEXT_ALIGN_LEFT);
  lv_obj_set_size(generic_action_, 132, 20);
  lv_obj_set_pos(generic_action_, 18, 158);

  for (std::size_t i = 0; i < 3U; ++i) {
    lv_obj_t* row = lv_obj_create(generic_);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(row, 146, 57);
    lv_obj_set_pos(row, 166, 49 + static_cast<int>(i) * 59);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(row, 0, 0);

    generic_stat_titles_[i] = lv_label_create(row);
    StyleLabel(generic_stat_titles_[i], desktop_font_, 0x6F7C90, LV_TEXT_ALIGN_LEFT);
    lv_obj_set_size(generic_stat_titles_[i], 130, 18);
    lv_obj_set_pos(generic_stat_titles_[i], 3, 4);

    generic_stat_values_[i] = lv_label_create(row);
    StyleLabel(generic_stat_values_[i], body_font_, 0xF3EEE6, LV_TEXT_ALIGN_LEFT);
    lv_obj_set_size(generic_stat_values_[i], 130, 23);
    lv_obj_set_pos(generic_stat_values_[i], 3, 26);

    if (i < 2U) {
      lv_obj_t* divider = lv_obj_create(row);
      lv_obj_clear_flag(divider, LV_OBJ_FLAG_SCROLLABLE);
      lv_obj_clear_flag(divider, LV_OBJ_FLAG_CLICKABLE);
      lv_obj_set_size(divider, 138, 1);
      lv_obj_set_pos(divider, 3, 55);
      lv_obj_set_style_border_width(divider, 0, 0);
      lv_obj_set_style_bg_color(divider, lv_color_hex(0x283548), 0);
      lv_obj_set_style_bg_opa(divider, LV_OPA_60, 0);
    }
  }
}

void LvglScreen::ShowHome() noexcept {
  const bool camera_was_active = page_ == Page::kCamera;
  StopOrbMotion();
  lv_obj_clear_flag(home_, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(voice_, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(camera_, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(wifi_provision_, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(generic_, LV_OBJ_FLAG_HIDDEN);
  page_ = Page::kHome;
  StartHomeOrbMotion();
  if (camera_was_active && camera_activity_handler_ != nullptr)
    camera_activity_handler_(false, camera_activity_user_data_);
}

void LvglScreen::ShowApp(std::size_t index) noexcept {
  if (index >= kAppCount) return;
  const bool camera_was_active = page_ == Page::kCamera;
  StopHomeOrbMotion();
  current_app_ = index;
  lv_obj_add_flag(home_, LV_OBJ_FLAG_HIDDEN);
  if (index == 0U) {
    lv_obj_clear_flag(voice_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(camera_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(wifi_provision_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(generic_, LV_OBJ_FLAG_HIDDEN);
    page_ = Page::kVoice;
    if (camera_was_active && camera_activity_handler_ != nullptr)
      camera_activity_handler_(false, camera_activity_user_data_);
    UpdateVoice();
    return;
  }
  StopOrbMotion();
  lv_obj_add_flag(voice_, LV_OBJ_FLAG_HIDDEN);
  if (index == 1U) {
    lv_obj_clear_flag(camera_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(wifi_provision_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(generic_, LV_OBJ_FLAG_HIDDEN);
    page_ = Page::kCamera;
    if (!camera_was_active && camera_activity_handler_ != nullptr)
      camera_activity_handler_(true, camera_activity_user_data_);
    return;
  }
  lv_obj_add_flag(camera_, LV_OBJ_FLAG_HIDDEN);
  if (camera_was_active && camera_activity_handler_ != nullptr)
    camera_activity_handler_(false, camera_activity_user_data_);
  if (index == 3U) {
    lv_obj_clear_flag(wifi_provision_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(generic_, LV_OBJ_FLAG_HIDDEN);
    page_ = Page::kWifiProvision;
    return;
  }
  lv_obj_add_flag(wifi_provision_, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(generic_, LV_OBJ_FLAG_HIDDEN);
  page_ = Page::kGeneric;
  const AppInfo& app = Apps()[index];
  lv_label_set_text(generic_title_, app.name);
  lv_label_set_text(generic_badge_, app.badge);
  lv_label_set_text(generic_icon_, app.icon);
  lv_label_set_text(generic_primary_, app.primary);
  lv_label_set_text(generic_secondary_, app.secondary);
  lv_label_set_text(generic_action_, app.action);
  for (std::size_t i = 0; i < 3U; ++i) {
    lv_label_set_text(generic_stat_titles_[i], app.stat_titles[i]);
    lv_label_set_text(generic_stat_values_[i], app.stat_values[i]);
  }
  lv_obj_set_style_text_color(generic_icon_,
                              lv_color_hex(index == 3U ? 0x4DA6FF :
                                           (index == 6U ? 0x667389 : 0xF3EEE6)), 0);
  UpdateClock();
}

void LvglScreen::OpenApp(std::size_t index) noexcept { ShowApp(index); }

void LvglScreen::SetVoiceInterruptHandler(VoiceInterruptHandler handler,
                                          void* user_data) noexcept {
  interrupt_handler_ = handler;
  interrupt_user_data_ = user_data;
}

void LvglScreen::SetAppActionHandler(AppActionHandler handler,
                                     void* user_data) noexcept {
  app_action_handler_ = handler;
  app_action_user_data_ = user_data;
}

void LvglScreen::SetCameraActivityHandler(CameraActivityHandler handler,
                                          void* user_data) noexcept {
  camera_activity_handler_ = handler;
  camera_activity_user_data_ = user_data;
}

void LvglScreen::SetVolumeChangeHandler(VolumeChangeHandler handler,
                                        void* user_data) noexcept {
  volume_change_handler_ = handler;
  volume_change_user_data_ = user_data;
}

void LvglScreen::SetVolume(const std::uint8_t percent) noexcept {
  volume_percent_ = std::min<std::uint8_t>(percent, 100U);
  if (volume_percent_ != 0U) volume_before_mute_ = volume_percent_;
  muted_ = volume_percent_ == 0U;

  if (voice_volume_slider_ != nullptr &&
      lv_slider_get_value(voice_volume_slider_) != volume_percent_) {
    lv_slider_set_value(voice_volume_slider_, volume_percent_, LV_ANIM_OFF);
  }
  if (voice_volume_value_ != nullptr) {
    char text[8]{};
    std::snprintf(text, sizeof(text), "%u%%",
                  static_cast<unsigned>(volume_percent_));
    lv_label_set_text(voice_volume_value_, text);
  }
  if (voice_mute_icon_ != nullptr) {
    lv_label_set_text(voice_mute_icon_,
                      muted_ ? kIconVolumeOff : kIconVolume);
  }
}

void LvglScreen::SetCameraFrame(const std::uint16_t* pixels,
                                std::size_t pixel_count) noexcept {
  if (camera_image_ == nullptr || pixels == nullptr ||
      pixel_count != camera_pixels_.size())
    return;
  std::memcpy(camera_pixels_.data(), pixels,
              camera_pixels_.size() * sizeof(camera_pixels_[0]));
  if (camera_status_ != nullptr) lv_label_set_text(camera_status_, "LIVE");
  lv_img_cache_invalidate_src(&camera_descriptor_);
  lv_obj_invalidate(camera_image_);
}

void LvglScreen::SetState(DeviceUiState state) noexcept {
  state_ = state;
  const bool conversation = state == DeviceUiState::kListening ||
                            state == DeviceUiState::kThinking ||
                            state == DeviceUiState::kSpeaking ||
                            state == DeviceUiState::kHappy ||
                            state == DeviceUiState::kSpeakingTail;
  if (conversation && page_ == Page::kHome) ShowApp(0U);
  UpdateVoice();
}

void LvglScreen::UpdateVoice() noexcept {
  if (voice_status_ == nullptr) return;
  const VoiceView& view = Voice(state_);
  lv_label_set_text(voice_status_, view.status);
  if (!has_text_) lv_label_set_text(voice_text_, view.hint);
  std::uint32_t accent = 0x315F9D;
  lv_opa_t recolor_opacity = LV_OPA_TRANSP;
  if (state_ == DeviceUiState::kListening) {
    accent = 0x35C9FF;
    recolor_opacity = LV_OPA_10;
  } else if (state_ == DeviceUiState::kThinking) {
    accent = 0x8B6CFF;
    recolor_opacity = LV_OPA_20;
  } else if (state_ == DeviceUiState::kSpeaking) {
    accent = 0x36D9C0;
    recolor_opacity = LV_OPA_10;
  } else if (state_ == DeviceUiState::kHappy) {
    accent = 0x48CFA4;
    recolor_opacity = LV_OPA_10;
  } else if (state_ == DeviceUiState::kSpeakingTail) {
    accent = 0x48CFA4;
    recolor_opacity = LV_OPA_10;
  } else if (state_ == DeviceUiState::kOffline) {
    accent = 0x56657A;
    recolor_opacity = LV_OPA_50;
  } else if (state_ == DeviceUiState::kError) {
    accent = 0xE9576C;
    recolor_opacity = LV_OPA_30;
  }
  if (voice_orb_image_ != nullptr) {
    lv_obj_set_style_img_recolor(voice_orb_image_, lv_color_hex(accent), 0);
    lv_obj_set_style_img_recolor_opa(voice_orb_image_, recolor_opacity, 0);
  }
  if (voice_orbits_[0] != nullptr) {
    lv_obj_set_style_border_color(voice_orbits_[0], lv_color_hex(accent), 0);
    lv_obj_set_style_border_opa(voice_orbits_[0], LV_OPA_50, 0);
  }
  if (voice_orbits_[1] != nullptr) {
    lv_obj_set_style_border_color(voice_orbits_[1], lv_color_hex(accent), 0);
    lv_obj_set_style_border_opa(voice_orbits_[1], LV_OPA_20, 0);
  }
  if (page_ == Page::kVoice) StartOrbMotion();
}

void LvglScreen::SetText(std::string_view first, std::string_view second) noexcept {
  if (voice_text_ == nullptr) return;
  std::string text(first);
  if (!second.empty()) {
    if (!text.empty()) text.push_back('\n');
    text.append(second);
  }
  // LVGL 8.2's FreeType renderer can report an unsupported character as a
  // placeholder without resolving a font.  Drawing that placeholder aborts
  // inside lv_font_get_glyph_bitmap().  Filter model-provided emoji and other
  // missing glyphs before handing the string to LVGL.
  text = SupportedText(small_font_, text);
  has_text_ = !text.empty();
  lv_label_set_text(voice_text_, has_text_ ? text.c_str() : Voice(state_).hint);
}

void LvglScreen::StartHomeOrbMotion() noexcept {
  StopHomeOrbMotion();
  if (home_orb_image_ == nullptr) return;

  lv_anim_t breathe{};
  lv_anim_init(&breathe);
  lv_anim_set_var(&breathe, home_orb_image_);
  lv_anim_set_exec_cb(&breathe, Zoom);
  lv_anim_set_values(&breathe, 112, 121);
  lv_anim_set_time(&breathe, 1900);
  lv_anim_set_playback_time(&breathe, 1900);
  lv_anim_set_repeat_count(&breathe, LV_ANIM_REPEAT_INFINITE);
  lv_anim_set_path_cb(&breathe, lv_anim_path_ease_in_out);
  lv_anim_start(&breathe);

  lv_anim_t drift = breathe;
  lv_anim_set_exec_cb(&drift, MoveX);
  lv_anim_set_values(&drift, -1, 1);
  lv_anim_set_time(&drift, 2300);
  lv_anim_set_playback_time(&drift, 2100);
  lv_anim_start(&drift);

  lv_anim_t rotate = breathe;
  lv_anim_set_exec_cb(&rotate, Rotate);
  lv_anim_set_values(&rotate, -45, 45);
  lv_anim_set_time(&rotate, 2900);
  lv_anim_set_playback_time(&rotate, 2700);
  lv_anim_start(&rotate);
}

void LvglScreen::StopHomeOrbMotion() noexcept {
  if (home_orb_image_ == nullptr) return;
  lv_anim_del(home_orb_image_, Zoom);
  lv_anim_del(home_orb_image_, MoveX);
  lv_anim_del(home_orb_image_, Rotate);
  lv_obj_set_style_translate_x(home_orb_image_, 0, 0);
  lv_img_set_zoom(home_orb_image_, 116);
  lv_img_set_angle(home_orb_image_, 0);
}

void LvglScreen::StartOrbMotion() noexcept {
  StopOrbMotion();
  if (voice_orb_image_ == nullptr) return;
  const VoiceView& view = Voice(state_);
  if (view.amplitude == 0) {
    lv_img_set_zoom(voice_orb_image_, 250);
    lv_obj_set_style_img_opa(voice_orb_image_, LV_OPA_60, 0);
    return;
  }

  const int drift = view.amplitude >= 8 ? 3 : (view.amplitude >= 4 ? 2 : 1);
  const int zoom = view.amplitude >= 8 ? 10 : (view.amplitude >= 4 ? 7 : 4);

  lv_anim_t breathe{};
  lv_anim_init(&breathe);
  lv_anim_set_var(&breathe, voice_orb_image_);
  lv_anim_set_exec_cb(&breathe, Zoom);
  lv_anim_set_values(&breathe, 256 - zoom, 256 + zoom);
  lv_anim_set_time(&breathe, view.period_ms);
  lv_anim_set_playback_time(&breathe, view.period_ms);
  lv_anim_set_repeat_count(&breathe, LV_ANIM_REPEAT_INFINITE);
  lv_anim_set_path_cb(&breathe, lv_anim_path_ease_in_out);
  lv_anim_start(&breathe);

  lv_anim_t x = breathe;
  lv_anim_set_exec_cb(&x, MoveX);
  lv_anim_set_values(&x, -drift, drift);
  lv_anim_set_time(&x, view.period_ms + 230U);
  lv_anim_set_playback_time(&x, view.period_ms + 170U);
  lv_anim_start(&x);

  lv_anim_t y = breathe;
  lv_anim_set_exec_cb(&y, MoveY);
  lv_anim_set_values(&y, drift, -drift);
  lv_anim_set_time(&y, view.period_ms + 410U);
  lv_anim_set_playback_time(&y, view.period_ms + 290U);
  lv_anim_start(&y);

  lv_anim_t rotate = breathe;
  lv_anim_set_exec_cb(&rotate, Rotate);
  const int angle = 18 + view.amplitude * 5;
  lv_anim_set_values(&rotate, -angle, angle);
  lv_anim_set_time(&rotate, view.period_ms + 530U);
  lv_anim_set_playback_time(&rotate, view.period_ms + 430U);
  lv_anim_start(&rotate);
}

void LvglScreen::StopOrbMotion() noexcept {
  if (voice_orb_image_ == nullptr) return;
  lv_anim_del(voice_orb_image_, Zoom);
  lv_anim_del(voice_orb_image_, MoveX);
  lv_anim_del(voice_orb_image_, MoveY);
  lv_anim_del(voice_orb_image_, Rotate);
  lv_obj_set_style_translate_x(voice_orb_image_, 0, 0);
  lv_obj_set_style_translate_y(voice_orb_image_, 0, 0);
  lv_img_set_zoom(voice_orb_image_, LV_IMG_ZOOM_NONE);
  lv_img_set_angle(voice_orb_image_, 0);
  lv_obj_set_style_img_opa(voice_orb_image_, LV_OPA_COVER, 0);
}

void LvglScreen::MoveX(void* object, std::int32_t value) {
  lv_obj_set_style_translate_x(static_cast<lv_obj_t*>(object), value, 0);
}

void LvglScreen::MoveY(void* object, std::int32_t value) {
  lv_obj_set_style_translate_y(static_cast<lv_obj_t*>(object), value, 0);
}

void LvglScreen::Zoom(void* object, std::int32_t value) {
  lv_img_set_zoom(static_cast<lv_obj_t*>(object), static_cast<std::uint16_t>(value));
}

void LvglScreen::Rotate(void* object, std::int32_t value) {
  lv_img_set_angle(static_cast<lv_obj_t*>(object), static_cast<std::int16_t>(value));
}

void LvglScreen::AppClicked(lv_event_t* event) {
  auto* screen = static_cast<LvglScreen*>(lv_event_get_user_data(event));
  lv_obj_t* target = lv_event_get_target(event);
  for (std::size_t i = 0; i < kAppCount; ++i) {
    if (screen->app_buttons_[i] == target) {
      screen->ShowApp(i);
      if (i == 0U && screen->app_action_handler_ != nullptr)
        screen->app_action_handler_(i, screen->app_action_user_data_);
      return;
    }
  }
}

void LvglScreen::BackClicked(lv_event_t* event) {
  static_cast<LvglScreen*>(lv_event_get_user_data(event))->ShowHome();
}

void LvglScreen::MuteClicked(lv_event_t* event) {
  auto* screen = static_cast<LvglScreen*>(lv_event_get_user_data(event));
  const std::uint8_t target = screen->volume_percent_ == 0U
                                  ? screen->volume_before_mute_
                                  : 0U;
  screen->SetVolume(target);
  if (screen->volume_change_handler_ != nullptr) {
    screen->volume_change_handler_(target, VolumeChangePhase::kCommit,
                                   screen->volume_change_user_data_);
  }
}

void LvglScreen::VolumeChanged(lv_event_t* event) {
  auto* screen = static_cast<LvglScreen*>(lv_event_get_user_data(event));
  const auto value = static_cast<std::uint8_t>(std::clamp(
      lv_slider_get_value(screen->voice_volume_slider_), 0, 100));
  screen->SetVolume(value);
  if (screen->volume_change_handler_ == nullptr) return;

  VolumeChangePhase phase = VolumeChangePhase::kPreview;
  if (lv_event_get_code(event) == LV_EVENT_PRESSED)
    phase = VolumeChangePhase::kBegin;
  else if (lv_event_get_code(event) == LV_EVENT_RELEASED ||
           lv_event_get_code(event) == LV_EVENT_PRESS_LOST)
    phase = VolumeChangePhase::kCommit;
  screen->volume_change_handler_(value, phase,
                                 screen->volume_change_user_data_);
}

void LvglScreen::VoiceClicked(lv_event_t* event) {
  auto* screen = static_cast<LvglScreen*>(lv_event_get_user_data(event));
  const bool interruptible = screen->state_ == DeviceUiState::kSpeaking ||
                             screen->state_ == DeviceUiState::kSpeakingTail;
  if (!interruptible) return;
  if (screen->interrupt_handler_ != nullptr) {
    screen->interrupt_handler_(screen->interrupt_user_data_);
  }
  if (screen->state_ == DeviceUiState::kSpeaking)
    screen->SetState(DeviceUiState::kListening);
}

void LvglScreen::AppActionClicked(lv_event_t* event) {
  auto* screen = static_cast<LvglScreen*>(lv_event_get_user_data(event));
  if (screen->app_action_handler_ != nullptr) {
    screen->app_action_handler_(screen->current_app_, screen->app_action_user_data_);
  }
}

void LvglScreen::ClockTick(lv_timer_t* timer) {
  static_cast<LvglScreen*>(timer->user_data)->UpdateClock();
}

void LvglScreen::UpdateClock() noexcept {
  if (home_clock_ == nullptr) return;
  const std::time_t now = std::time(nullptr);
  std::tm local{};
  localtime_r(&now, &local);
  char text[8]{};
  std::snprintf(text, sizeof(text), "%02d:%02d", local.tm_hour, local.tm_min);
  SetLabelIfChanged(home_clock_, text);
  if (home_date_ != nullptr) {
    static constexpr std::array<const char*, 7> weekdays{{
        "星期日", "星期一", "星期二", "星期三", "星期四", "星期五", "星期六"}};
    char date[32]{};
    std::snprintf(date, sizeof(date), "%d月%d日 · %s", local.tm_mon + 1,
                  local.tm_mday, weekdays[static_cast<std::size_t>(local.tm_wday)]);
    SetLabelIfChanged(home_date_, date);
  }
  if (home_year_ != nullptr) {
    char year[16]{};
    std::snprintf(year, sizeof(year), "%d年", local.tm_year + 1900);
    SetLabelIfChanged(home_year_, year);
  }
  if (page_ == Page::kGeneric && current_app_ == 2U && generic_primary_ != nullptr) {
    SetLabelIfChanged(generic_primary_, text);
  }
}

void LvglScreen::Destroy() noexcept {
  StopHomeOrbMotion();
  StopOrbMotion();
  if (clock_timer_ != nullptr) {
    lv_timer_del(clock_timer_);
    clock_timer_ = nullptr;
  }
  if (lv_scr_act() != nullptr) lv_obj_clean(lv_scr_act());
  if (icon_small_font_ != nullptr) lv_ft_font_destroy(icon_small_font_);
  if (icon_font_ != nullptr) lv_ft_font_destroy(icon_font_);
  if (clock_font_ != nullptr) lv_ft_font_destroy(clock_font_);
  if (title_font_ != nullptr) lv_ft_font_destroy(title_font_);
  if (body_font_ != nullptr) lv_ft_font_destroy(body_font_);
  if (small_font_ != nullptr) lv_ft_font_destroy(small_font_);
  if (home_caption_font_ != nullptr) lv_ft_font_destroy(home_caption_font_);
  if (desktop_font_ != nullptr) lv_ft_font_destroy(desktop_font_);
  clock_font_ = title_font_ = body_font_ = small_font_ = nullptr;
  home_caption_font_ = desktop_font_ = nullptr;
  icon_font_ = icon_small_font_ = nullptr;
  if (freetype_initialized_) {
    lv_freetype_destroy();
    freetype_initialized_ = false;
  }
  home_ = voice_ = camera_ = wifi_provision_ = generic_ = nullptr;
  home_clock_ = home_date_ = home_year_ = nullptr;
  home_orb_image_ = nullptr;
  for (auto*& button : app_buttons_) button = nullptr;
  voice_orb_image_ = nullptr;
  for (auto*& orbit : voice_orbits_) orbit = nullptr;
  voice_status_ = voice_text_ = voice_mute_icon_ = nullptr;
  voice_volume_slider_ = voice_volume_value_ = nullptr;
  camera_image_ = camera_status_ = nullptr;
  generic_title_ = generic_hero_ = generic_badge_ = generic_icon_ = nullptr;
  generic_primary_ = generic_secondary_ = generic_action_ = nullptr;
  for (auto*& title : generic_stat_titles_) title = nullptr;
  for (auto*& value : generic_stat_values_) value = nullptr;
  has_text_ = false;
  muted_ = false;
  volume_percent_ = 60U;
  volume_before_mute_ = 60U;
  interrupt_handler_ = nullptr;
  interrupt_user_data_ = nullptr;
  app_action_handler_ = nullptr;
  app_action_user_data_ = nullptr;
  camera_activity_handler_ = nullptr;
  camera_activity_user_data_ = nullptr;
  volume_change_handler_ = nullptr;
  volume_change_user_data_ = nullptr;
  camera_descriptor_ = {};
  current_app_ = 0U;
  page_ = Page::kHome;
  state_ = DeviceUiState::kIdle;
}

}  // namespace boompi::ui
