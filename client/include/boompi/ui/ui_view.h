#pragma once
#include <array>
#include <cstdint>
#include <cstring>
#include <string_view>

namespace boompi::ui {
enum class DeviceUiState : std::uint8_t {
  kIdle,
  kListening,
  kThinking,
  kSpeaking,
  kHappy,
  kOffline,
  kError,
};
enum class UiActionKind : std::uint8_t { Wake, Interrupt, Volume };
struct UiAction {
  UiActionKind kind{UiActionKind::Wake};
  std::uint8_t volume{60};
};

// application 与 UI worker 交换完整快照；固定字幕缓冲避免跨线程动态分配。
struct UiView {
  DeviceUiState state{DeviceUiState::kOffline};
  std::uint8_t volume{60};
  std::array<char, 127> text{};

  void ClearText() noexcept {
    text.fill('\0');
  }
  void AppendText(std::string_view delta) noexcept {
    if (delta.size() >= text.size() - 1) {
      delta.remove_prefix(delta.size() - (text.size() - 1));
      ClearText();
    }
    // 丢弃旧前缀时回到 UTF-8 字符边界，不能把续字节作为新字幕的开头。
    while (!delta.empty() && (static_cast<unsigned char>(delta.front()) & 0xc0U) == 0x80U) {
      delta.remove_prefix(1);
    }
    const std::size_t used = std::strlen(text.data());
    std::size_t drop = used + delta.size() > 126 ? used + delta.size() - 126 : 0;
    while (drop < used && (static_cast<unsigned char>(text[drop]) & 0xc0U) == 0x80U) {
      ++drop;
    }
    const std::size_t kept = used - drop;
    std::memmove(text.data(), text.data() + drop, kept);
    if (!delta.empty()) {
      std::memcpy(text.data() + kept, delta.data(), delta.size());
    }
    text[kept + delta.size()] = '\0';
  }
};
}  // namespace boompi::ui
