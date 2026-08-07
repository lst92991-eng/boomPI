/**
 * @file main.cpp
 * @brief 用 SDL 在 PC 上承载与 RV1106 完全相同的 LvglScreen 页面。
 *
 * 模拟器只替换显示 flush 和 pointer 输入端口，页面构建、事件规则、字体过滤与摄像头
 * 占位图仍使用生产代码。它既可打开交互窗口，也可无窗口导出 frame.bmp，方便学生
 * 在没有开发板时检查 320x240 排版。整个程序单线程调用 LVGL，符合板端所有权约束。
 */
#include <SDL.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#include <lvgl.h>

#include "boompi/ui/lvgl_screen.h"

namespace {

constexpr int kWidth = 320;
constexpr int kHeight = 240;
// 这些端口状态只由主循环和 LVGL 同步回调访问，无需为模拟器引入线程同步。
std::array<std::uint16_t, kWidth * kHeight> g_frame{};
int g_pointer_x = 0;
int g_pointer_y = 0;
bool g_pointer_pressed = false;
bool g_frame_dirty = true;

/**
 * @brief 把 LVGL 脏矩形复制到完整 RGB565 软件帧缓冲。
 *
 * 与板端 Flush 的 SPI 旋转实现不同，SDL 使用页面原生 320x240 坐标；两端都在复制
 * 完成后调用 lv_disp_flush_ready()，页面层无需了解显示后端。
 */
void Flush(lv_disp_drv_t* driver, const lv_area_t* area, lv_color_t* source) {
  static_cast<void>(driver);
  for (int y = area->y1; y <= area->y2; ++y) {
    for (int x = area->x1; x <= area->x2; ++x) {
      g_frame[static_cast<std::size_t>(y) * kWidth + x] = source++->full;
    }
  }
  g_frame_dirty = true;
  lv_disp_flush_ready(driver);
}

/** @brief 把 2 倍缩放 SDL 窗口的鼠标状态转换成 LVGL pointer 样本。 */
void ReadPointer(lv_indev_drv_t*, lv_indev_data_t* data) {
  data->point.x = static_cast<lv_coord_t>(std::clamp(g_pointer_x, 0, kWidth - 1));
  data->point.y = static_cast<lv_coord_t>(std::clamp(g_pointer_y, 0, kHeight - 1));
  data->state = g_pointer_pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

/**
 * @brief 将当前 RGB565 页面导出为稳定名称的 BMP 预览。
 *
 * 先写 frame.tmp.bmp 再 rename，外部截图观察者只会读到完整帧，不会撞上 SDL 正在
 * 写入的半成品；像素掩码与板端 RGB565 布局保持一致。
 */
bool SaveFrame(const std::string& directory) {
  SDL_Surface* surface = SDL_CreateRGBSurfaceFrom(
      g_frame.data(), kWidth, kHeight, 16, kWidth * 2,
      0xF800, 0x07E0, 0x001F, 0);
  if (surface == nullptr) {
    return false;
  }
  const std::string temporary = directory + "/frame.tmp.bmp";
  const std::string output = directory + "/frame.bmp";
  const bool ok = SDL_SaveBMP(surface, temporary.c_str()) == 0 &&
                  std::rename(temporary.c_str(), output.c_str()) == 0;
  SDL_FreeSurface(surface);
  return ok;
}

}  // namespace

int main(int argc, char** argv) {
  std::string preview_dir;
  bool demo_voice = false;
  int demo_app = -1;
  // 当前 Ubuntu 的 DroidSansFallbackFull 缺少拉丁数字，LVGL 8.2 软件渲染缺失字形时
  // 会把时钟字符交给占位 glyph 并触发异常。Noto Sans CJK 同时覆盖中文和拉丁字符。
  const char* font = "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc";
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--preview-dir") == 0 && i + 1 < argc) {
      preview_dir = argv[++i];
    } else if (std::strcmp(argv[i], "--font") == 0 && i + 1 < argc) {
      font = argv[++i];
    } else if (std::strcmp(argv[i], "--demo-voice") == 0) {
      demo_voice = true;
    } else if (std::strcmp(argv[i], "--demo-app") == 0 && i + 1 < argc) {
      demo_app = std::atoi(argv[++i]);
    }
  }
  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) {
    return 1;
  }
  SDL_Window* window = SDL_CreateWindow(
      "boomPI LVGL 320x240", SDL_WINDOWPOS_CENTERED,
      SDL_WINDOWPOS_CENTERED, kWidth * 2, kHeight * 2,
      // 导出预览时隐藏窗口，CI 或远程 VM 仍可复用同一渲染路径。
      preview_dir.empty() ? SDL_WINDOW_SHOWN : SDL_WINDOW_HIDDEN);
  SDL_Renderer* renderer =
      window == nullptr
          ? nullptr
          : SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
  SDL_Texture* texture =
      renderer == nullptr
          ? nullptr
          : SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGB565,
                              SDL_TEXTUREACCESS_STREAMING, kWidth, kHeight);
  if (preview_dir.empty() && texture == nullptr) {
    return 2;
  }

  lv_init();
  // 32 行 draw buffer 模拟板端分块刷屏，避免 PC 的全帧内存掩盖脏矩形相关问题。
  static std::array<lv_color_t, kWidth * 32> draw_pixels{};
  static lv_disp_draw_buf_t draw_buffer;
  lv_disp_draw_buf_init(&draw_buffer, draw_pixels.data(), nullptr, draw_pixels.size());
  static lv_disp_drv_t display;
  lv_disp_drv_init(&display);
  display.hor_res = kWidth;
  display.ver_res = kHeight;
  display.flush_cb = Flush;
  display.draw_buf = &draw_buffer;
  lv_disp_drv_register(&display);
  static lv_indev_drv_t pointer;
  lv_indev_drv_init(&pointer);
  pointer.type = LV_INDEV_TYPE_POINTER;
  pointer.read_cb = ReadPointer;
  lv_indev_drv_register(&pointer);

  // LvglScreen 从此处开始只接触抽象 display/input，页面代码无法区分 SDL 与 RV1106。
  boompi::ui::LvglScreen screen;
  if (!screen.Create(font)) {
    return 3;
  }
  constexpr std::array states{
      boompi::ui::DeviceUiState::kIdle, boompi::ui::DeviceUiState::kListening,
      boompi::ui::DeviceUiState::kThinking, boompi::ui::DeviceUiState::kSpeaking,
      boompi::ui::DeviceUiState::kHappy, boompi::ui::DeviceUiState::kOffline,
      boompi::ui::DeviceUiState::kError, boompi::ui::DeviceUiState::kSpeakingTail};
  std::size_t state = 0;
  auto apply_state = [&] {
    screen.SetState(states[state]);
    if (states[state] == boompi::ui::DeviceUiState::kSpeaking) {
      // 保留三个曾导致板端 LVGL 崩溃的 emoji 作为固定回归样本；页面需要过滤缺失字形
      // 并继续绘制剩余文字。
      screen.SetText("当然可以，正在为你查询。😅", "北京今天适合外出。😄😂");
    } else {
      screen.SetText({}, {});
    }
  };
  if (demo_voice) {
    state = 1U;
    apply_state();
  } else if (demo_app >= 0) {
    screen.OpenApp(static_cast<std::size_t>(demo_app));
  }
  auto next_state = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  bool running = true;
  while (running) {
    // 单次循环同时驱动 SDL、LVGL tick 和渲染，所有页面对象保持单线程顺序访问。
    const auto started = std::chrono::steady_clock::now();
    SDL_Event event{};
    while (SDL_PollEvent(&event)) {
      if (event.type == SDL_QUIT) {
        running = false;
      } else if (event.type == SDL_MOUSEMOTION) {
        // SDL 窗口按 2 倍显示，输入坐标在进入 LVGL 前还原到 320x240。
        g_pointer_x = event.motion.x / 2;
        g_pointer_y = event.motion.y / 2;
      } else if (event.type == SDL_MOUSEBUTTONDOWN || event.type == SDL_MOUSEBUTTONUP) {
        g_pointer_x = event.button.x / 2;
        g_pointer_y = event.button.y / 2;
        g_pointer_pressed = event.type == SDL_MOUSEBUTTONDOWN;
      } else if (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_SPACE) {
        // 空格手动轮换语音状态，便于课堂逐页观察而无需连接服务端。
        state = (state + 1U) % states.size();
        apply_state();
      }
    }
    if (demo_voice && started >= next_state) {
      // --demo-voice 每两秒轮换状态，用于连续观察状态映射和页面稳定性。
      state = (state + 1U) % states.size();
      apply_state();
      next_state = started + std::chrono::seconds(2);
    }
    lv_tick_inc(16);
    lv_timer_handler();
    if (texture != nullptr && g_frame_dirty) {
      // SDL 纹理只在 LVGL flush 产生新内容时上传，空闲页面不会重复复制整帧。
      SDL_UpdateTexture(texture, nullptr, g_frame.data(), kWidth * 2);
      SDL_RenderClear(renderer);
      SDL_RenderCopy(renderer, texture, nullptr, nullptr);
      SDL_RenderPresent(renderer);
    }
    if (!preview_dir.empty() && g_frame_dirty && !SaveFrame(preview_dir)) {
      return 3;
    }
    g_frame_dirty = false;
    std::this_thread::sleep_until(started + std::chrono::milliseconds(16));
  }
  // 页面先销毁，再释放承载它的 SDL display 资源，顺序与板端 Close() 保持一致。
  screen.Destroy();
  SDL_DestroyTexture(texture);
  SDL_DestroyRenderer(renderer);
  SDL_DestroyWindow(window);
  SDL_Quit();
  return 0;
}
