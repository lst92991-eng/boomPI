/** @file lvgl_screen.cpp
 * @brief 小智页面：表情、状态、字幕、开始/停止和音量，全部控件在本文件创建。
 *
 * open 创建一次，show 只更新变化；点击通过 handler_cb 交给应用作业务决策。
 * 运行期只有 UI 线程调用这里的函数，页面不持有声卡或网络连接。
 */
#include "boompi/ui/lvgl_screen.h"

#include <ft2build.h>
#include <lvgl.h>
#include FT_FREETYPE_H

#include <algorithm>
#include <cstring>

#include "twemoji_64.h"

namespace ui_page
{
static lv_obj_t *voice_page{}, *face{}, *status{}, *hint{}, *subtitle{};
static lv_obj_t *talk{}, *talk_text{}, *slider{}, *volume_text{};
static lv_font_t *font{};
static Handler handler_cb{};
static ui::DeviceUiState voice_state{ui::DeviceUiState::Idle};
static bool shown{false};
/** @brief 每个显示状态的固定文案和表情；排列与 ui::DeviceUiState 一致。 */
struct VoiceView
{
    const char *title;
    const char *hint;
    const lv_img_dsc_t *face;
};
static const VoiceView views[] = {{"准备好了", "说出唤醒词或点开始", &emoji_1f642_64},
                                  {"正在聆听", "请说，我在听", &emoji_1f62f_64},
                                  {"正在思考", "可轻触停止等待", &emoji_1f914_64},
                                  {"正在回答", "可以直接说话插话", &emoji_1f606_64},
                                  {"连接已断开", "正在尝试重新连接", &emoji_1f614_64},
                                  {"暂时遇到问题", "详情请查看终端日志", &emoji_1f614_64}};
/** @brief 创建同一字体/颜色的标签，尺寸由调用点按页面布局指定。 */
static lv_obj_t *label(lv_obj_t *parent, const char *text, int x, int y, int w, int h)
{
    auto *object = lv_label_create(parent);
    lv_obj_set_pos(object, x, y);
    lv_obj_set_size(object, w, h);
    lv_obj_set_style_text_font(object, font, 0);
    lv_obj_set_style_text_color(object, lv_color_hex(0x263B46), 0);
    lv_label_set_text(object, text);
    return object;
}

/** @brief 根据当前画面发出开始或停止意图；真正是否允许操作由应用再按实时状态判断。 */
static void click_cb(lv_event_t *)
{
    if (voice_state == ui::DeviceUiState::Speaking ||
        voice_state == ui::DeviceUiState::Thinking)
    {
        handler_cb(Event::Interrupt, 0);
    }
    else if (voice_state == ui::DeviceUiState::Idle)
    {
        handler_cb(Event::Wake, 0);
    }
}

static void show_volume(std::uint8_t value)
{
    if (value == 0)
    {
        lv_label_set_text(volume_text, "静音");
    }
    else
    {
        lv_label_set_text_fmt(volume_text, "音量  %u%%", value);
    }
}

/** @brief 拖动实时改音量，释放/失去按压时才请求保存，使实时调节和配置持久化各自按合适频率执行。
 */
static void volume_changed_cb(lv_event_t *event)
{
    const auto code = lv_event_get_code(event);
    if (code == LV_EVENT_VALUE_CHANGED || code == LV_EVENT_RELEASED ||
        code == LV_EVENT_PRESS_LOST)
    {
        const auto value = static_cast<std::uint8_t>(lv_slider_get_value(slider));
        show_volume(value);
        handler_cb(code == LV_EVENT_VALUE_CHANGED ? Event::Volume : Event::SaveVolume, value);
    }
}

/** @brief 查询当前字体的字形覆盖情况，作为字幕过滤和字体初始化的依据。 */
static bool has_glyph(std::uint32_t codepoint)
{
    lv_font_glyph_dsc_t glyph{};
    return lv_font_get_glyph_dsc(font, &glyph, codepoint, 0) && glyph.resolved_font;
}

/** @brief 过滤字体不支持的字符并更新字幕；复用快照大小的固定缓冲，不创建动态字符串。 */
static void show_subtitle(const ui::UiView &view)
{
    auto text = view.text;
    std::size_t used = 0;
    // 字幕保留换行和字体支持的字符，保证每个送入LVGL的码点都有可绘制字形。
    for (std::uint32_t at = 0; view.text[at];)
    {
        const auto start = at;
        const auto codepoint = _lv_txt_encoded_next(view.text.data(), &at);
        if (codepoint == '\n' || (codepoint >= 32 && has_glyph(codepoint)))
        {
            std::memcpy(text.data() + used, view.text.data() + start, at - start);
            used += at - start;
        }
    }
    text[used] = '\0';
    if (std::strcmp(lv_label_get_text(subtitle), text.data()) != 0)
    {
        lv_label_set_text(subtitle, text.data());
    }
}

/** @brief 创建占满屏幕、不可滚动的根容器，子控件随它一起释放。 */
static lv_obj_t *container()
{
    auto *object = lv_obj_create(lv_scr_act());
    lv_obj_set_size(object, 320, 240);
    lv_obj_set_style_pad_all(object, 0, 0);
    lv_obj_set_style_border_width(object, 0, 0);
    lv_obj_set_style_radius(object, 0, 0);
    lv_obj_set_style_bg_color(object, lv_color_hex(0xF0F5F7), 0);
    lv_obj_clear_flag(object, LV_OBJ_FLAG_SCROLLABLE);
    return object;
}

/** @brief 创建文字按钮并绑定点击回调；只设置页面样式，不决定对话状态。 */
static lv_obj_t *button(lv_obj_t *parent, const char *text, int x, int y, int width, int height)
{
    auto *object = lv_btn_create(parent);
    lv_obj_set_pos(object, x, y);
    lv_obj_set_size(object, width, height);
    lv_obj_set_style_bg_color(object, lv_color_hex(0x087F8C), 0);
    lv_obj_set_style_radius(object, 9, 0);
    lv_obj_set_style_shadow_width(object, 0, 0);
    auto *caption = label(object, text, 0, 0, width, 22);
    lv_obj_set_style_text_color(caption, lv_color_white(), 0);
    lv_obj_set_style_text_align(caption, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(caption);
    lv_obj_add_event_cb(object, click_cb, LV_EVENT_CLICKED, nullptr);
    return object;
}

bool open(const char *font_path, Handler callback_cb)
{
    close();
    // lv_init持有进程级FreeType缓存，本页面只创建和释放自身使用的字体对象。
    // 先验证字体文件和中文字形，再交给LVGL创建字体对象，使失败资源能在此完整释放。
    FT_Library library{};
    FT_Face face_check{};
    if (!font_path || FT_Init_FreeType(&library) != 0)
    {
        return false;
    }
    const bool valid = FT_New_Face(library, font_path, 0, &face_check) == 0 &&
                       FT_Set_Pixel_Sizes(face_check, 0, 16) == 0 &&
                       FT_Get_Char_Index(face_check, 0x667A) != 0;
    if (face_check)
    {
        FT_Done_Face(face_check);
    }
    FT_Done_FreeType(library);
    lv_ft_info_t settings{};
    settings.name = font_path;
    settings.weight = 16;
    settings.style = FT_FONT_STYLE_NORMAL;
    if (!valid || !lv_ft_font_init(&settings))
    {
        return false;
    }
    font = settings.font;
    if (!has_glyph(0x667A))
    {
        close();
        return false;
    }
    handler_cb = callback_cb;
    // 仅创建小智页面，运行时只更新状态、字幕和音量。
    voice_page = container();
    label(voice_page, "boomPI · 小智", 16, 12, 175, 24);
    auto *card = lv_obj_create(voice_page);
    lv_obj_set_pos(card, 12, 43);
    lv_obj_set_size(card, 296, 139);
    lv_obj_set_style_radius(card, 12, 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    face = lv_img_create(voice_page);
    lv_obj_set_pos(face, 26, 49);
    lv_obj_add_flag(face, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(face, click_cb, LV_EVENT_CLICKED, nullptr);
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
    lv_obj_add_event_cb(slider, volume_changed_cb, LV_EVENT_ALL, nullptr);
    shown = false;
    show(ui::UiView{});
    return true;
}

void show(const ui::UiView &view)
{
    if (!shown || voice_state != view.state)
    {
        voice_state = view.state;
        const auto &voice = views[static_cast<unsigned>(view.state)];
        lv_img_set_src(face, voice.face);
        lv_label_set_text(status, voice.title);
        lv_label_set_text(hint, voice.hint);
        const bool busy = voice_state == ui::DeviceUiState::Thinking ||
                          voice_state == ui::DeviceUiState::Speaking;
        lv_label_set_text(talk_text, busy ? "停止" : "开始对话");
        if (busy || voice_state == ui::DeviceUiState::Idle)
        {
            lv_obj_clear_state(talk, LV_STATE_DISABLED);
        }
        else
        {
            lv_obj_add_state(talk, LV_STATE_DISABLED);
        }
    }
    show_subtitle(view);
    const auto level = std::min<std::uint8_t>(view.volume, 100);
    if (!shown ||
        (!lv_obj_has_state(slider, LV_STATE_PRESSED) && lv_slider_get_value(slider) != level))
    {
        lv_slider_set_value(slider, level, LV_ANIM_OFF);
        show_volume(level);
    }
    shown = true;
}

void close()
{
    // 页面先于字体释放；外部必须先停UI线程。
    if (voice_page)
    {
        lv_obj_del(voice_page);
    }
    voice_page = nullptr;
    if (font)
    {
        lv_ft_font_destroy(font);
        font = nullptr;
    }
    handler_cb = nullptr;
}
}  // namespace ui_page
