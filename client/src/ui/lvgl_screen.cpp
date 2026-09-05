/**
 * @file lvgl_screen.cpp
 * @brief 定义 boomPI 在模拟器和 RV1106 上共用的 320x240 LVGL 页面。
 *
 * 本文件只描述页面结构、显示状态和用户事件，不执行 GPIO、网络、文件或摄像头
 * I/O。调用者必须在单一 LVGL 所有者线程中调用全部接口，事件回调也会同步运行在
 * 该线程。这样页面代码可以脱离 BSP 在 SDL 中复现，同时维持板端线程边界清晰。
 */
#include "boompi/ui/lvgl_screen.h"

#include <lvgl.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <ctime>
#include <new>
#include <string>

#include "twemoji_64.h"

namespace boompi::ui {
namespace {

constexpr std::uint32_t kBackground = 0x0A1220;
constexpr std::uint32_t kText = 0xF3F6FA;
constexpr std::uint32_t kMuted = 0x8492A6;
constexpr std::uint32_t kBlue = 0x4DA6FF;
using Page = LvglScreen::Page;
struct AppEntry {
  const char* title;
  Page page;
};
// 排列顺序只影响桌面位置，事件直接携带页面身份。
constexpr std::array<AppEntry, 4> kApps{{{"小智", Page::kVoice},
                                         {"摄像头", Page::kCamera},
                                         {"时间", Page::kClock},
                                         {"WiFi", Page::kWifi}}};

/** @brief 把 application 状态映射成静态表情与默认文案，页面不复制业务状态机。 */
struct VoiceView final {
  const char* title;
  const char* hint;
  const lv_img_dsc_t* face;
};

const std::array<VoiceView, 7> kVoiceViews{{
    {"随时可以说话", "说出唤醒词开始对话", &emoji_1f642_64},
    {"正在聆听", "我在听，请继续", &emoji_1f62f_64},
    {"正在思考", "正在组织回答", &emoji_1f914_64},
    {"正在回答", "轻触表情即可打断", &emoji_1f606_64},
    {"回答完成", "随时可以继续问我", &emoji_1f642_64},
    {"暂时离线", "等待网络恢复", &emoji_1f614_64},
    {"发生错误", "请检查网络后重试", &emoji_1f614_64},
}};

constexpr std::array<const char*, 4> kCameraStatus{{"OFF", "START", "LIVE", "ERROR"}};

/** @brief 统一创建固定尺寸文本，保证 320x240 各页面使用同一排版基线。 */
lv_obj_t* Label(lv_obj_t* parent, const char* value, const lv_font_t* font, std::uint32_t color,
                int x, int y, int width, int height) {
  lv_obj_t* label = lv_label_create(parent);
  lv_label_set_text(label, value);
  lv_obj_set_style_text_font(label, font, 0);
  lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
  lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_pos(label, x, y);
  lv_obj_set_size(label, width, height);
  return label;
}

/** @brief 在提交文本前查询 FreeType 字体能否解析指定 Unicode codepoint。 */
bool HasGlyph(const lv_font_t* font, std::uint32_t codepoint) {
  lv_font_glyph_dsc_t glyph{};
  return lv_font_get_glyph_dsc(font, &glyph, codepoint, 0U) && glyph.resolved_font != nullptr;
}

/**
 * @brief 过滤字体缺失字符，保持 UTF-8 codepoint 边界完整。
 *
 * Qwen 文本可能包含 emoji，而板端 CJK 字体通常不覆盖这些字形。LVGL 8.2 的软件
 * 渲染器曾在缺失字形上失败，因此这里整码点过滤，同时保留换行供字幕分段显示。
 */
std::string SupportedText(const lv_font_t* font, const std::string& input) {
  std::string output;
  for (std::uint32_t offset = 0U; offset < input.size();) {
    const std::uint32_t start = offset;
    const std::uint32_t codepoint = _lv_txt_encoded_next(input.c_str(), &offset);
    if (codepoint == '\n' || (codepoint >= 0x20U && HasGlyph(font, codepoint))) {
      output.append(input, start, offset - start);
    }
  }
  return output;
}

/** @brief 通过 LVGL FreeType 端口加载指定像素字号，所有权交给 LvglScreen::Impl。 */
lv_font_t* LoadFont(const char* path, std::uint16_t size) {
  lv_ft_info_t info{};
  info.name = path;
  info.weight = size;
  info.style = FT_FONT_STYLE_NORMAL;
  return lv_ft_font_init(&info) ? info.font : nullptr;
}

}  // namespace

struct LvglScreen::Impl final {
  static constexpr std::size_t kPixelCount = 320U * 180U;

  struct AppClick final {
    Impl* owner;
    Page page;
  };

  // 所有 lv_obj_t/lv_timer_t 指针仅在 LVGL 所有者线程访问，页面切换后按 Begin() 重置。
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
  // 事件回调同步转交给 DeviceUi；回调内不得长期阻塞 LVGL timer handler。
  EventHandler event_handler{};
  void* event_data{};
  lv_img_dsc_t camera_descriptor{};
  // 页面持有自己的 RGB565 副本，camera worker 的缓冲区生命周期不会泄漏进 LVGL。
  std::array<std::uint16_t, kPixelCount> pixels{};
  std::string subtitle_text;
  Page page{Page::kHome};
  DeviceUiState voice_state{DeviceUiState::kIdle};
  CameraStatus camera_state{CameraStatus::Stopped};
  unsigned fps_tenths{};
  std::uint8_t volume{60U};

  /** @brief 同步发布页面意图，value 仅用于 0..100 音量百分比。 */
  void Emit(Event event, std::uint8_t value = 0U) {
    if (event_handler != nullptr) {
      event_handler(event, value, event_data);
    }
  }

  /**
   * @brief 清空活动 screen 并建立新页面的共同背景。
   *
   * lv_obj_clean() 会递归删除旧页面对象，因此只有跨页面仍需访问的成员指针需要在
   * 此处显式失效。其余控件会在对应 Build*() 中重新赋值。
   */
  void Begin(Page next) {
    lv_obj_clean(lv_scr_act());
    provision_info = nullptr;
    lv_obj_set_style_bg_color(lv_scr_act(), lv_color_hex(kBackground), 0);
    lv_obj_set_style_bg_opa(lv_scr_act(), LV_OPA_COVER, 0);
    page = next;
  }

  /** @brief 为子页面建立统一返回区和标题，返回事件仍在 LVGL 线程内顺序处理。 */
  void Header(const char* title) {
    lv_obj_t* back = Label(lv_scr_act(), "<", text_font, kText, 0, 0, 44, 38);
    lv_obj_set_style_pad_top(back, 7, 0);
    lv_obj_add_flag(back, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(back, BackClicked, LV_EVENT_CLICKED, this);
    Label(lv_scr_act(), title, text_font, kText, 100, 8, 120, 24);
  }

  /** @brief 重建桌面对象树，并为每个按钮绑定稳定存储的页面上下文。 */
  void BuildHome() {
    Begin(Page::kHome);
    Label(lv_scr_act(), "boomPI", text_font, kText, 10, 8, 80, 24);
    clock = Label(lv_scr_act(), "", text_font, kText, 235, 8, 70, 24);
    for (std::size_t index = 0U; index < kApps.size(); ++index) {
      const int x = 8 + static_cast<int>(index % 2U) * 156;
      const int y = 43 + static_cast<int>(index / 2U) * 94;
      lv_obj_t* button = lv_btn_create(lv_scr_act());
      lv_obj_set_pos(button, x, y);
      lv_obj_set_size(button, 148, 86);
      app_clicks[index] = {this, kApps[index].page};
      lv_obj_add_event_cb(button, AppClicked, LV_EVENT_CLICKED, &app_clicks[index]);
      Label(button, kApps[index].title, text_font, kText, 2, 31, 140, 24);
    }
    UpdateClock();
  }

  /**
   * @brief 建立小智表情、字幕和音量交互页面。
   *
   * 表情点击只在 Speaking 产生打断；滑块拖动持续预览 gain，释放时
   * 再提交持久化。页面重建会恢复 Impl 中保存的状态、字幕和音量。
   */
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

  /**
   * @brief 建立 SC3336 固定尺寸预览页并先显示 START 状态。
   *
   * 页面创建后由 ShowApp() 发出 CameraOn，实际打开 /dev/video14 的工作留给
   * DeviceUi camera worker；camera_descriptor 始终引用本对象拥有的像素数组。
   */
  void BuildCamera() {
    Begin(Page::kCamera);
    camera_state = CameraStatus::Starting;
    Header("SC3336");
    camera_info = Label(lv_scr_act(), "", text_font, kBlue, 160, 7, 152, 24);
    camera_image = lv_img_create(lv_scr_act());
    lv_img_set_src(camera_image, &camera_descriptor);
    lv_obj_set_pos(camera_image, 0, 30);
    RenderCamera();
  }

  /**
   * @brief 建立 Wi-Fi 配网页。
   *
   * Wi-Fi 二维码编码临时热点的固定 SSID/密码，手机扫码后连接 boomPI-Setup；
   * AppClicked 随后发布 Provision，板端脚本负责真正启动热点和保存用户网络凭据。
   * 页面文字只显示过程结果，绝不接收或记录用户 Wi-Fi 密码。
   */
  void BuildWifi() {
    Begin(Page::kWifi);
    Header("WiFi");
    lv_obj_t* qr =
        lv_qrcode_create(lv_scr_act(), 104, lv_color_hex(0x08111C), lv_color_hex(0xFFFFFF));
    constexpr char payload[] = "WIFI:T:WPA;S:boomPI-Setup;P:boompi-setup;;";
    lv_obj_set_pos(qr, 108, 48);
    lv_qrcode_update(qr, payload, sizeof(payload) - 1U);
    Label(lv_scr_act(), "boomPI-Setup", text_font, kText, 60, 166, 200, 24);
    provision_info =
        Label(lv_scr_act(), "扫描二维码连接配网热点", text_font, kMuted, 30, 197, 260, 24);
  }

  void BuildClock() {
    Begin(Page::kClock);
    Header("时间");
    clock = Label(lv_scr_act(), "", text_font, kText, 30, 105, 260, 30);
    UpdateClock();
  }

  /**
   * @brief 根据明确的页面身份切换，并发布摄像头资源边界事件。
   *
   * CameraOn/CameraOff 紧跟页面切换，使进入页面才占用 ISP，离开页面立即释放；
   * 已经位于摄像头页时不重复发 CameraOn，避免 UI worker 等待仍在运行的采集线程。
   */
  void ShowApp(Page next) {
    const bool was_camera = page == Page::kCamera;
    switch (next) {
      case Page::kHome:
        BuildHome();
        break;
      case Page::kVoice:
        BuildVoice();
        break;
      case Page::kCamera:
        BuildCamera();
        break;
      case Page::kClock:
        BuildClock();
        break;
      case Page::kWifi:
        BuildWifi();
        break;
      default:
        return;
    }
    if (was_camera && next != Page::kCamera) {
      Emit(Event::kCameraOff);
    }
    if (!was_camera && next == Page::kCamera) {
      Emit(Event::kCameraOn);
    }
  }

  /** @brief 保存全局音量并在语音页存在时同步滑块，关闭动画避免回调抖动。 */
  void SetVolume(std::uint8_t percent) {
    volume = std::min<std::uint8_t>(percent, 100U);
    if (page != Page::kVoice) {
      return;
    }
    lv_slider_set_value(slider, volume, LV_ANIM_OFF);
  }

  /** @brief 将当前语音状态和可选服务端字幕投影到既有控件。 */
  void RenderVoice() {
    if (page != Page::kVoice) {
      return;
    }
    const VoiceView& view = kVoiceViews[static_cast<std::size_t>(voice_state)];
    lv_img_set_src(face, view.face);
    lv_label_set_text_fmt(subtitle, "%s\n%s", view.title,
                          subtitle_text.empty() ? view.hint : subtitle_text.c_str());
  }

  /**
   * @brief 更新摄像头状态、实测显示 FPS 和图像脏区。
   *
   * camera_descriptor 的地址保持不变，像素内容原地更新；主动失效 LVGL 图片缓存后
   * 再 invalidate 控件，防止缓存继续显示上一帧或错误状态前的旧画面。
   */
  void RenderCamera() {
    if (page != Page::kCamera) {
      return;
    }
    if (camera_state == CameraStatus::Live) {
      lv_obj_clear_flag(camera_image, LV_OBJ_FLAG_HIDDEN);
      lv_label_set_text_fmt(camera_info, "LIVE %u.%u FPS", fps_tenths / 10U, fps_tenths % 10U);
    } else {
      lv_obj_add_flag(camera_image, LV_OBJ_FLAG_HIDDEN);
      lv_label_set_text(camera_info, kCameraStatus[static_cast<std::size_t>(camera_state)]);
    }
    lv_img_cache_invalidate_src(&camera_descriptor);
    lv_obj_invalidate(camera_image);
  }

  /** @brief 只在桌面和时钟页更新本地时钟，避免已销毁 clock 指针被定时器访问。 */
  void UpdateClock() {
    if (page != Page::kHome && page != Page::kClock) {
      return;
    }
    const std::time_t now = std::time(nullptr);
    std::tm local{};
    localtime_r(&now, &local);
    lv_label_set_text_fmt(clock, "%02d:%02d", local.tm_hour, local.tm_min);
  }

  /** @brief 返回桌面，并在离开摄像头页后立即请求释放采集资源。 */
  static void BackClicked(lv_event_t* event) {
    auto* self = static_cast<Impl*>(lv_event_get_user_data(event));
    self->ShowApp(Page::kHome);
  }

  /**
   * @brief 打开目标页面，并把小智点击和唤醒词汇入同一 application 状态机。
   *
   * Wi-Fi 页面在控件创建完成后才发布 Provision，配网脚本的启动结果可以直接回写
   * 当前页面上的 provision_info 标签。
   */
  static void AppClicked(lv_event_t* event) {
    auto* click = static_cast<AppClick*>(lv_event_get_user_data(event));
    click->owner->ShowApp(click->page);
    if (click->page == Page::kVoice) {
      click->owner->Emit(Event::kWake);
    }
    if (click->page == Page::kWifi) {
      click->owner->Emit(Event::kProvision);
    }
  }

  /** @brief 仅在扬声器仍有有效回复时把表情点击解释为打断意图。 */
  static void FaceClicked(lv_event_t* event) {
    Impl* self = static_cast<Impl*>(lv_event_get_user_data(event));
    const bool speaking = self->voice_state == DeviceUiState::kSpeaking;
    if (speaking) {
      self->Emit(Event::kInterrupt);
    } else {
      self->Emit(Event::kWake);
    }
  }

  /**
   * @brief 区分连续音量预览与手势结束后的持久化提交。
   *
   * VALUE_CHANGED 可以让用户即时听到增益变化；RELEASED/PRESS_LOST 每次手势只产生
   * 一次 commit，使 DeviceUi 能把闪存写入频率限制到实际操作次数。
   */
  static void SliderChanged(lv_event_t* event) {
    const lv_event_code_t code = lv_event_get_code(event);
    const bool committed = code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST;
    if (code != LV_EVENT_VALUE_CHANGED && !committed) {
      return;
    }
    Impl* self = static_cast<Impl*>(lv_event_get_user_data(event));
    self->SetVolume(static_cast<std::uint8_t>(lv_slider_get_value(self->slider)));
    const Event change = committed ? Event::kVolumeCommit : Event::kVolumePreview;
    self->Emit(change, self->volume);
  }
};

bool LvglScreen::Create(const char* font_path) noexcept {
  Destroy();
  impl_ = new (std::nothrow) Impl;
  if (impl_ == nullptr) {
    return false;
  }
  // FreeType 缓存规模固定，避免 64 MiB 板端内存因动态页面数量产生不可预测增长。
  if (font_path == nullptr || !lv_freetype_init(2, 4, 65536)) {
    delete impl_;
    impl_ = nullptr;
    return false;
  }
  impl_->text_font = LoadFont(font_path, 16);
  // 用“小智”的“智”验证中文覆盖，提前拒绝只能显示拉丁字符的字体。
  if (impl_->text_font == nullptr || !HasGlyph(impl_->text_font, 0x667AU)) {
    Destroy();
    return false;
  }
  impl_->camera_descriptor.header.cf = LV_IMG_CF_TRUE_COLOR;
  impl_->camera_descriptor.header.w = 320;
  impl_->camera_descriptor.header.h = 180;
  impl_->camera_descriptor.data_size = impl_->pixels.size() * sizeof(std::uint16_t);
  impl_->camera_descriptor.data = reinterpret_cast<const std::uint8_t*>(impl_->pixels.data());
  // 分钟级 timer 只更新桌面时钟，首次内容由 BuildHome() 立即写入。
  impl_->timer = lv_timer_create(
      [](lv_timer_t* timer) {
        static_cast<Impl*>(timer->user_data)->UpdateClock();
      },
      60000, impl_);
  impl_->BuildHome();
  return true;
}

void LvglScreen::OpenApp(Page page) noexcept {
  impl_->ShowApp(page);
}

void LvglScreen::SetEventHandler(EventHandler handler, void* data) noexcept {
  impl_->event_handler = handler;
  impl_->event_data = data;
}

void LvglScreen::SetVolume(std::uint8_t percent) noexcept {
  impl_->SetVolume(percent);
}

void LvglScreen::SetProvisionMessage(const char* text, bool error) noexcept {
  if (impl_ == nullptr || impl_->provision_info == nullptr) {
    return;
  }
  lv_label_set_text(impl_->provision_info, text);
  lv_obj_set_style_text_color(impl_->provision_info, lv_color_hex(error ? 0xFF6B6B : kMuted),
                              0);
}

void LvglScreen::SetCameraStatus(CameraStatus state) noexcept {
  if (impl_ == nullptr) {
    return;
  }
  impl_->camera_state = state;
  if (state != CameraStatus::Live) {
    // 错误或停止时清空旧像素，用户不会把冻结的最后一帧误认为实时画面。
    impl_->fps_tenths = 0U;
    impl_->pixels.fill(0U);
  }
  impl_->RenderCamera();
}

void LvglScreen::SetCameraFrame(const std::uint16_t* pixels, std::size_t pixel_count,
                                unsigned fps_tenths) noexcept {
  if (impl_ == nullptr || pixels == nullptr || pixel_count != impl_->pixels.size()) {
    return;
  }
  std::memcpy(impl_->pixels.data(), pixels, pixel_count * sizeof(pixels[0]));
  impl_->fps_tenths = fps_tenths;
  impl_->RenderCamera();
}

void LvglScreen::SetState(DeviceUiState state) noexcept {
  if (impl_ == nullptr) {
    return;
  }
  impl_->voice_state = state;
  const bool active = state >= DeviceUiState::kListening && state <= DeviceUiState::kHappy;
  // 语音从唤醒词启动时自动进入小智页；用户主动浏览其他页面时不强制抢占页面。
  if (active && impl_->page == Page::kHome) {
    impl_->BuildVoice();
  }
  impl_->RenderVoice();
}

void LvglScreen::SetText(std::string_view first, std::string_view second) noexcept {
  if (impl_ == nullptr) {
    return;
  }
  try {
    std::string text(first);
    if (!text.empty() && !second.empty()) {
      text.push_back('\n');
    }
    text.append(second);
    impl_->subtitle_text = SupportedText(impl_->text_font, text);
  } catch (...) {
    impl_->subtitle_text.clear();
  }
  impl_->RenderVoice();
}

void LvglScreen::Destroy() noexcept {
  if (impl_ == nullptr) {
    return;
  }
  // timer 可能引用 Impl，必须先删除；字体在页面对象清空后才能安全释放。
  if (impl_->timer != nullptr) {
    lv_timer_del(impl_->timer);
  }
  lv_obj_clean(lv_scr_act());
  if (impl_->text_font != nullptr) {
    lv_ft_font_destroy(impl_->text_font);
  }
  lv_freetype_destroy();
  delete impl_;
  impl_ = nullptr;
}

}  // namespace boompi::ui
