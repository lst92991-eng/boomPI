// 实际UI线程与LVGL注册/回收；只替换硬件、页面内容及摄像头I/O。
#include <lvgl.h>

#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>

#include "boompi/ui/device_ui.h"
#include "../src/platform/rv1106/display_touch.h"
#include "../src/ui/camera_capture.h"

namespace {
std::atomic<unsigned> shown{0};
bool page_ok = true;
lv_obj_t* root{};
boompi::ui::page::Image pixels;
}
namespace boompi::platform::rv1106 {
bool DisplayTouch::Open() {
  return true;
}
void DisplayTouch::Close() noexcept {}
bool DisplayTouch::Flush(const lv_area_t&, const lv_color_t*) {
  return true;
}
void DisplayTouch::ReadInput(lv_indev_data_t* data) {
  data->state = LV_INDEV_STATE_REL;
}
}
namespace boompi::ui::page {
bool open(const char*, Handler) {
  if (!page_ok) {
    return false;
  }
  root = lv_obj_create(lv_scr_act());
  return true;
}
void show(const UiView&) {
  ++shown;
}
Image& pixels() noexcept {
  return ::pixels;
}
void present(CameraStatus, bool) {}
void close() noexcept {
  if (root) {
    lv_obj_del(root);
    root = nullptr;
  }
}
}
namespace boompi::ui::camera_capture {
bool open() {
  return true;
}
bool read(page::Image&, CameraStatus& status) {
  status = CameraStatus::Stopped;
  return false;
}
void close() noexcept {}
}
int main() {
  namespace ui = boompi::ui;
  bool ok = true;
  for (unsigned cycle = 0; cycle < 5; ++cycle) {
    page_ok = false;
    ok = !ui::open() && ok;
    ui::close();
    page_ok = true;
    if (!ui::open()) {
      ok = false;
      break;
    }
    const auto previous = shown.load();
    ui::UiView view;
    view.AppendText("UI lifecycle");
    ui::show(view);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (shown.load() == previous && std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    ok = shown.load() > previous && ok;
    ui::close();
    ui::close();
    ok = lv_disp_get_next(nullptr) == nullptr && lv_indev_get_next(nullptr) == nullptr && ok;
  }
  ui::close();
  lv_img_cache_set_size(0);
  lv_freetype_destroy();
  if (!ok) {
    std::cerr << "UI lifecycle failed\n";
  }
  return ok ? 0 : 1;
}
