// 真LVGL/FreeType，无声卡、网络或硬件；固定页面切换与事件回归。
#include <lvgl.h>

#include <array>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>

#include "boompi/ui/lvgl_screen.h"

namespace {
using namespace boompi::ui;
std::vector<page::Event> events;
std::array<lv_color_t, 320 * 32> draw;
std::array<std::uint16_t, 320 * 240> screen;
void require(bool ok, const char* text) {
  if (!ok) {
    throw std::runtime_error(text);
  }
}
unsigned objects(lv_obj_t* parent) {
  unsigned count = 1;
  for (unsigned i = 0; i < lv_obj_get_child_cnt(parent); ++i) {
    count += objects(lv_obj_get_child(parent, static_cast<int>(i)));
  }
  return count;
}
lv_obj_t* find(lv_obj_t* parent, const lv_obj_class_t* type) {
  if (lv_obj_check_type(parent, type)) {
    return parent;
  }
  for (unsigned i = 0; i < lv_obj_get_child_cnt(parent); ++i) {
    if (auto* result = find(lv_obj_get_child(parent, static_cast<int>(i)), type)) {
      return result;
    }
  }
  return nullptr;
}
void save(const std::string& path) {
  lv_refr_now(nullptr);
  std::ofstream file(path, std::ios::binary);
  file << "P6\n320 240\n255\n";
  for (auto p : screen) {
    const char rgb[] = {static_cast<char>(((p >> 11) & 31) * 255 / 31),
                       static_cast<char>(((p >> 5) & 63) * 255 / 63),
                       static_cast<char>((p & 31) * 255 / 31)};
    file.write(rgb, 3);
  }
  require(file.good(), "preview write failed");
}
}  // namespace
int main(int argc, char** argv) {
  using namespace boompi::ui;
  lv_init();
  lv_disp_draw_buf_t buffer;
  lv_disp_draw_buf_init(&buffer, draw.data(), nullptr, draw.size());
  lv_disp_drv_t driver;
  lv_disp_drv_init(&driver);
  driver.hor_res = 320;
  driver.ver_res = 240;
  driver.draw_buf = &buffer;
  driver.flush_cb = [](lv_disp_drv_t* d, const lv_area_t* area, lv_color_t* pixels) {
    for (int y = area->y1; y <= area->y2; ++y) {
      for (int x = area->x1; x <= area->x2; ++x) {
        screen[y * 320 + x] = pixels++->full;
      }
    }
    lv_disp_flush_ready(d);
  };
  auto* display = lv_disp_drv_register(&driver);
  try {
    const char* font =
        argc > 1 ? argv[1] : "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc";
    require(!page::open("/missing-font-for-test"), "missing font accepted");
    require(!page::open(nullptr), "null font accepted");
    require(!page::open("/dev/null"), "invalid font accepted");
    for (int cycle = 0; cycle < 3; ++cycle) {
      require(page::open(font,
                         [](page::Event e, std::uint8_t) {
                           events.push_back(e);
                         }),
              "page init");
      const auto count = objects(lv_scr_act());
      auto* face = find(lv_scr_act(), &lv_img_class);
      UiView view;
      view.state = DeviceUiState::Idle;
      page::show(view);
      lv_event_send(face, LV_EVENT_CLICKED, nullptr);
      require(events.back() == page::Event::Wake, "face wake");
      view.state = DeviceUiState::Speaking;
      view.AppendText("正在回答。😅\n保留中文，过滤不支持的字符。😂");
      page::show(view);
      lv_event_send(face, LV_EVENT_CLICKED, nullptr);
      require(events.back() == page::Event::Interrupt, "face interrupt");
      auto* slider = find(lv_scr_act(), &lv_slider_class);
      lv_slider_set_value(slider, 42, LV_ANIM_OFF);
      lv_event_send(slider, LV_EVENT_VALUE_CHANGED, nullptr);
      require(events.back() == page::Event::Volume, "volume preview");
      lv_event_send(slider, LV_EVENT_RELEASED, nullptr);
      require(events.back() == page::Event::SaveVolume, "volume commit");
      if (argc > 2 && cycle == 0) {
        save(std::string(argv[2]) + "/voice.ppm");
      }
      for (int i = 0; i < 100; ++i) {
        auto before = events.size();
        page::camera(true);
        page::camera(true);
        require(events.size() == before + 1 && events.back() == page::Event::CameraOn,
                "duplicate camera start");
        page::pixels().fill(0x07E0);
        page::present(CameraStatus::Live, true);
        lv_refr_now(display);
        page::present(CameraStatus::Error, false);
        page::show(view);
        page::camera(false);
        require(events.back() == page::Event::CameraOff, "camera exit");
        require(objects(lv_scr_act()) == count, "page objects grew during switching");
      }
      if (argc > 2 && cycle == 0) {
        page::camera(true);
        page::pixels().fill(0x07E0);
        page::present(CameraStatus::Live, true);
        save(std::string(argv[2]) + "/camera.ppm");
      }
      page::close();
      page::close();
      require(lv_obj_get_child_cnt(lv_scr_act()) == 0, "page cleanup");
    }
    lv_disp_remove(display);
    driver.draw_ctx_deinit(&driver, driver.draw_ctx);
    lv_mem_free(driver.draw_ctx);
    lv_img_cache_set_size(0);
    lv_freetype_destroy();  // 进程退出，整个测试期间只初始化过一次。
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    page::close();
    lv_disp_remove(display);
    driver.draw_ctx_deinit(&driver, driver.draw_ctx);
    lv_mem_free(driver.draw_ctx);
    lv_img_cache_set_size(0);
    lv_freetype_destroy();
    return 1;
  }
}
