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

#ifndef BOOMPI_ICON_FONT_PATH
#define BOOMPI_ICON_FONT_PATH ""
#endif

namespace {

constexpr int kWidth = 320, kHeight = 240;
std::array<std::uint16_t, kWidth * kHeight> g_frame{};
int g_pointer_x = 0, g_pointer_y = 0;
bool g_pointer_pressed = false;
bool g_frame_dirty = true;

void Flush(lv_disp_drv_t* driver, const lv_area_t* area, lv_color_t* source) {
  static_cast<void>(driver);
  for (int y = area->y1; y <= area->y2; ++y)
    for (int x = area->x1; x <= area->x2; ++x)
      g_frame[static_cast<std::size_t>(y) * kWidth + x] = source++->full;
  g_frame_dirty = true;
  lv_disp_flush_ready(driver);
}

void ReadPointer(lv_indev_drv_t*, lv_indev_data_t* data) {
  data->point.x = static_cast<lv_coord_t>(std::clamp(g_pointer_x, 0, kWidth - 1));
  data->point.y = static_cast<lv_coord_t>(std::clamp(g_pointer_y, 0, kHeight - 1));
  data->state = g_pointer_pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

bool SaveFrame(const std::string& directory) {
  SDL_Surface* surface = SDL_CreateRGBSurfaceFrom(g_frame.data(), kWidth, kHeight, 16,
                                                   kWidth * 2, 0xF800, 0x07E0, 0x001F, 0);
  if (surface == nullptr) return false;
  const std::string temporary = directory + "/frame.tmp.bmp";
  const std::string output = directory + "/frame.bmp";
  const bool ok = SDL_SaveBMP(surface, temporary.c_str()) == 0 && std::rename(temporary.c_str(), output.c_str()) == 0;
  SDL_FreeSurface(surface);
  return ok;
}

}  // namespace

int main(int argc, char** argv) {
  std::string preview_dir;
  bool demo_voice = false;
  int demo_app = -1;
  // DroidSansFallbackFull lacks Latin digits in this Ubuntu image.  LVGL 8.2
  // treats the missing digit as a placeholder glyph, so the clock would abort
  // in the software renderer.  Noto Sans CJK covers both Chinese and Latin.
  const char* font = "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc";
  const char* icon_font = BOOMPI_ICON_FONT_PATH;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--preview-dir") == 0 && i + 1 < argc) preview_dir = argv[++i];
    else if (std::strcmp(argv[i], "--font") == 0 && i + 1 < argc) font = argv[++i];
    else if (std::strcmp(argv[i], "--icon-font") == 0 && i + 1 < argc) icon_font = argv[++i];
    else if (std::strcmp(argv[i], "--demo-voice") == 0) demo_voice = true;
    else if (std::strcmp(argv[i], "--demo-app") == 0 && i + 1 < argc) demo_app = std::atoi(argv[++i]);
  }
  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) return 1;
  SDL_Window* window = SDL_CreateWindow("boomPI LVGL 320x240", SDL_WINDOWPOS_CENTERED,
                                        SDL_WINDOWPOS_CENTERED, kWidth * 2, kHeight * 2,
                                        preview_dir.empty() ? SDL_WINDOW_SHOWN : SDL_WINDOW_HIDDEN);
  SDL_Renderer* renderer = window == nullptr ? nullptr : SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
  SDL_Texture* texture = renderer == nullptr ? nullptr : SDL_CreateTexture(
      renderer, SDL_PIXELFORMAT_RGB565, SDL_TEXTUREACCESS_STREAMING, kWidth, kHeight);
  if (preview_dir.empty() && texture == nullptr) return 2;

  lv_init();
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

  boompi::ui::LvglScreen screen;
  if (!screen.Create(font, icon_font)) return 3;
  constexpr std::array states{
      boompi::ui::DeviceUiState::kIdle, boompi::ui::DeviceUiState::kListening,
      boompi::ui::DeviceUiState::kThinking, boompi::ui::DeviceUiState::kSpeaking,
      boompi::ui::DeviceUiState::kHappy, boompi::ui::DeviceUiState::kOffline,
      boompi::ui::DeviceUiState::kError};
  std::size_t state = 0;
  auto apply_state = [&] {
    screen.SetState(states[state]);
    if (states[state] == boompi::ui::DeviceUiState::kSpeaking) {
      // Keep the three emoji that previously crashed LVGL on the board in the
      // simulator fixture.  The UI must omit unsupported glyphs and keep drawing.
      screen.SetText("当然可以，正在为你查询。😅", "新加坡今天适合外出。😄😂");
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
    const auto started = std::chrono::steady_clock::now();
    SDL_Event event{};
    while (SDL_PollEvent(&event)) {
      if (event.type == SDL_QUIT) running = false;
      else if (event.type == SDL_MOUSEMOTION) {
        g_pointer_x = event.motion.x / 2;
        g_pointer_y = event.motion.y / 2;
      } else if (event.type == SDL_MOUSEBUTTONDOWN || event.type == SDL_MOUSEBUTTONUP) {
        g_pointer_x = event.button.x / 2;
        g_pointer_y = event.button.y / 2;
        g_pointer_pressed = event.type == SDL_MOUSEBUTTONDOWN;
      } else if (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_SPACE) {
        state = (state + 1U) % states.size();
        apply_state();
      }
    }
    if (demo_voice && started >= next_state) {
      state = (state + 1U) % states.size();
      apply_state();
      next_state = started + std::chrono::seconds(2);
    }
    lv_tick_inc(16);
    lv_timer_handler();
    if (texture != nullptr && g_frame_dirty) {
      SDL_UpdateTexture(texture, nullptr, g_frame.data(), kWidth * 2);
      SDL_RenderClear(renderer);
      SDL_RenderCopy(renderer, texture, nullptr, nullptr);
      SDL_RenderPresent(renderer);
    }
    if (!preview_dir.empty() && g_frame_dirty && !SaveFrame(preview_dir)) return 3;
    g_frame_dirty = false;
    std::this_thread::sleep_until(started + std::chrono::milliseconds(16));
  }
  screen.Destroy();
  SDL_DestroyTexture(texture);
  SDL_DestroyRenderer(renderer);
  SDL_DestroyWindow(window);
  SDL_Quit();
  return 0;
}
