#pragma once
#include <array>
#include <cstdint>
#include <cstring>
#include <string_view>

namespace boompi::ui {
enum class DeviceUiState : std::uint8_t {
  kIdle, kListening, kThinking, kSpeaking, kHappy, kOffline, kError,
};
enum class UiActionKind : std::uint8_t { Wake, Interrupt, Volume };
struct UiAction {
  UiActionKind kind{UiActionKind::Wake};
  std::uint8_t volume{60};
};

// 一个完整显示快照。字幕固定大小，不在跨线程边界分配字符串。
struct UiView {
  DeviceUiState state{DeviceUiState::kOffline};
  std::uint8_t volume{60};
  std::array<char, 127> text{};

  void ClearText() noexcept { text.fill('\0'); }
  void AppendText(std::string_view delta) noexcept {
    if (delta.size() >= text.size() - 1) {
      delta.remove_prefix(delta.size() - (text.size() - 1));
      ClearText();
    }
    while (!delta.empty() && (static_cast<unsigned char>(delta.front()) & 0xc0U) == 0x80U)
      delta.remove_prefix(1);
    const std::size_t used = std::strlen(text.data());
    std::size_t drop = used + delta.size() > 126 ? used + delta.size() - 126 : 0;
    while (drop < used && (static_cast<unsigned char>(text[drop]) & 0xc0U) == 0x80U) ++drop;
    const std::size_t kept = used - drop;
    std::memmove(text.data(), text.data() + drop, kept);
    if (!delta.empty()) std::memcpy(text.data() + kept, delta.data(), delta.size());
    text[kept + delta.size()] = '\0';
  }
};
}  // namespace boompi::ui
