#include "boompi/ui/device_ui.h"

#include <fcntl.h>
#include <lvgl.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <thread>

#include "../platform/rv1106/display_touch.h"
#include "boompi/ui/lvgl_screen.h"
#include "camera_capture.h"

namespace boompi::ui {
namespace {
constexpr char kSettings[] = "/userdata/boompi/config/ui.settings";
constexpr char kTemporary[] = "/userdata/boompi/config/ui.settings.tmp";
constexpr char kFont[] = "/userdata/boompi/fonts/NotoSansCJK-Regular.ttc";
constexpr char kFallbackFont[] = "/oem/usr/share/simsun_en.ttf";
platform::rv1106::DisplayTouch hardware;
std::thread worker;
std::atomic<bool> stopping{false};
std::atomic<int> action{-1}, volume{-1};
std::mutex mutex;
std::condition_variable changed;
UiView view;
bool dirty{true};
lv_disp_t* display{};
lv_indev_t* input{};
lv_disp_draw_buf_t draw_buffer;
lv_disp_drv_t output;
lv_indev_drv_t pointer;
std::array<lv_color_t, 320 * 32> draw_pixels;
void save_volume(std::uint8_t value) {
  // 仅在释放滑块时提交；临时文件完整写入后才替换正式配置。
  const int fd =
      ::open(kTemporary, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
  std::FILE* file = fd < 0 ? nullptr : fdopen(fd, "w");
  bool ok =
      file && std::fprintf(file, "%u\n", value) > 0 && std::fflush(file) == 0 && fsync(fd) == 0;
  if (file) {
    ok = std::fclose(file) == 0 && ok;
  } else if (fd >= 0) {
    ::close(fd);
  }
  if (!ok || std::rename(kTemporary, kSettings) != 0) {
    unlink(kTemporary);
    std::fprintf(stderr, "boompi-ui: volume save failed\n");
  }
}
void event(page::Event type, std::uint8_t value) {
  if (type == page::Event::CameraOn) {
    camera_capture::open();
  } else if (type == page::Event::CameraOff) {
    camera_capture::close();
  } else if (type == page::Event::Volume || type == page::Event::SaveVolume) {
    volume.store(value);
    if (type == page::Event::SaveVolume) {
      save_volume(value);
    }
  } else {
    action.store(static_cast<int>(type == page::Event::Wake ? UiActionKind::Wake
                                                            : UiActionKind::Interrupt));
  }
}
void flush(lv_disp_drv_t* driver, const lv_area_t* area, lv_color_t* pixels) {
  if (!hardware.Flush(*area, pixels)) {
    stopping.store(true);
  }
  lv_disp_flush_ready(driver);
}
void run() {
  static_cast<void>(nice(5));
  auto tick = std::chrono::steady_clock::now();
  try {
    while (!stopping.load()) {
      // 快照只在短锁内复制；渲染、字体、SPI和摄像头I/O不持应用交接锁。
      UiView next;
      bool update;
      {
        std::lock_guard<std::mutex> lock(mutex);
        update = dirty;
        if (update) {
          next = view;
        }
        dirty = false;
      }
      if (update) {
        page::show(next);
      }
      CameraStatus status;
      const bool frame = camera_capture::read(page::pixels(), status);
      page::present(status, frame);
      const auto now = std::chrono::steady_clock::now();
      lv_tick_inc(static_cast<std::uint32_t>(
          std::chrono::duration_cast<std::chrono::milliseconds>(now - tick).count()));
      tick = now;
      lv_timer_handler();
      std::unique_lock<std::mutex> lock(mutex);
      changed.wait_for(lock, std::chrono::milliseconds(30), [] {
        return stopping.load() || dirty;
      });
    }
  } catch (...) {
    stopping.store(true);
    std::fprintf(stderr, "boompi-ui: display worker failed\n");
  }
  camera_capture::close();
}
}  // namespace
std::uint8_t load_volume(std::uint8_t fallback) noexcept {
  unsigned value = std::min<std::uint8_t>(fallback, 100);
  if (auto* file = std::fopen(kSettings, "r")) {
    unsigned saved;
    if (std::fscanf(file, "%u", &saved) == 1 && saved <= 100) {
      value = saved;
    }
    std::fclose(file);
  }
  return static_cast<std::uint8_t>(value);
}
bool open() {
  close();
  if (!hardware.Open()) {
    return false;
  }
  // 尚未启动工作线程，此处依次配置LVGL端口、字体、两页；不需要启动回执。
  lv_init();
  lv_disp_draw_buf_init(&draw_buffer, draw_pixels.data(), nullptr, draw_pixels.size());
  lv_disp_drv_init(&output);
  output.hor_res = 320;
  output.ver_res = 240;
  output.draw_buf = &draw_buffer;
  output.flush_cb = flush;
  display = lv_disp_drv_register(&output);
  lv_indev_drv_init(&pointer);
  pointer.type = LV_INDEV_TYPE_POINTER;
  pointer.read_cb = [](lv_indev_drv_t*, lv_indev_data_t* data) {
    hardware.ReadInput(data);
  };
  input = lv_indev_drv_register(&pointer);
  try {
    const char* font = access(kFont, R_OK) == 0 ? kFont : kFallbackFont;
    if (!display || !input || !page::open(font, event)) {
      close();
      return false;
    }
    view = {};
    dirty = true;
    action.store(-1);
    volume.store(-1);
    stopping.store(false);
    worker = std::thread(run);
    return true;
  } catch (...) {
    close();
    return false;
  }
}
void show(const UiView& value) noexcept {
  std::lock_guard<std::mutex> lock(mutex);
  view = value;
  dirty = true;
  changed.notify_one();
}
bool poll_action(UiAction& result) noexcept {
  const int command = action.exchange(-1);
  if (command >= 0) {
    result.kind = static_cast<UiActionKind>(command);
    return true;
  }
  const int percent = volume.exchange(-1);
  if (percent < 0) {
    return false;
  }
  result = {UiActionKind::Volume, static_cast<std::uint8_t>(percent)};
  return true;
}
void close() noexcept {
  stopping.store(true);
  changed.notify_one();
  if (worker.joinable()) {
    worker.join();
  }
  camera_capture::close();
  // join后不再有回调；按页面→输入/显示→硬件释放，允许失败阶段调用。
  page::close();
  if (input) {
    lv_indev_delete(input);
    input = nullptr;
  }
  if (display) {
    lv_disp_remove(display);
    display = nullptr;
  }
  // LVGL8.2的disp_remove不释放自动分配的draw_ctx，由端口持有者收尾。
  if (output.draw_ctx) {
    output.draw_ctx_deinit(&output, output.draw_ctx);
    lv_mem_free(output.draw_ctx);
    output.draw_ctx = nullptr;
  }
  hardware.Close();
}
}  // namespace boompi::ui
