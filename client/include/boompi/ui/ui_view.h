/**
 * @file ui_view.h
 * @brief application 与 UI 之间按值传递的显示快照和用户动作。
 *
 * 应用拥有对话状态及字幕累积；ui::show复制快照，由UI线程渲染。
 * UiAction 沿相反方向传递意图，页面不分配 generation，也不决定录音或回复的生命周期。
 */
#pragma once
#include <array>
#include <cstdint>
#include <cstring>
#include <string_view>

namespace ui
{
/**
 * @brief 应用四态的显示投影，另含网络离线和错误提示。
 *
 * Listening共用等待开口和正在上传的画面，WaitingReply显示Thinking。
 * 枚举次序对应 lvgl_screen.cpp 的静态表情表，只能传入这里定义的有效值。
 */
enum class DeviceUiState : std::uint8_t
{
    Idle,
    Listening,
    Thinking,
    Speaking,
    Offline,
    Error,
};
/** @brief 应用消费语音与音量动作。 */
enum class UiActionKind : std::uint8_t
{
    Wake,
    Interrupt,
    Volume
};
/** @brief 一个已合并的用户动作，volume 仅对 Volume 有效，单位为百分比。 */
struct UiAction
{
    UiActionKind kind{UiActionKind::Wake};
    std::uint8_t volume{60};
};

/**
 * @brief 一份可直接复制的状态、音量和字幕快照。
 *
 * text 最多保留 126 字节 UTF-8 文本，最后一字节为 NUL；上限按字节而非汉字数计算。
 * AppendText()在应用中累积流式文本，UI只在短锁内复制整个结构。
 * 这里不含互斥量，也不允许生产者和消费者同时读写同一个实例。
 */
struct UiView
{
    DeviceUiState state{DeviceUiState::Offline};
    std::uint8_t volume{60};
    std::array<char, 127> text{};

    /** @brief 开始新回答时清空字幕，使显示内容与当前轮次对应。 */
    void ClearText()
    {
        text.fill('\0');
    }
    /**
   * @brief 追加一个流式字幕片段，空间不足时丢弃最旧前缀，保留最近内容。
   * @param delta 调用期间有效的 UTF-8 片段，调用后不保留其引用。
   *
   * 裁剪点沿UTF-8字符边界移动，保留完整字符；上游提供已校验的UTF-8文本。
   * 本函数仅在固定数组中整理字幕，UI线程随后通过快照取得结果。
   */
    void AppendText(std::string_view delta)
    {
        if (delta.size() >= text.size() - 1)
        {
            delta.remove_prefix(delta.size() - (text.size() - 1));
            ClearText();
        }
        // 从完整UTF-8字符开始保留新片段，使字幕开头可直接解码。
        while (!delta.empty() && (static_cast<unsigned char>(delta.front()) & 0xc0U) == 0x80U)
        {
            delta.remove_prefix(1);
        }
        // 新片段本身已受容量约束；只移动仍能容纳的旧尾部，再追加，始终留出终止符。
        const std::size_t used = std::strlen(text.data());
        std::size_t drop = used + delta.size() > 126 ? used + delta.size() - 126 : 0;
        while (drop < used && (static_cast<unsigned char>(text[drop]) & 0xc0U) == 0x80U)
        {
            ++drop;
        }
        const std::size_t kept = used - drop;
        std::memmove(text.data(), text.data() + drop, kept);
        if (!delta.empty())
        {
            std::memcpy(text.data() + kept, delta.data(), delta.size());
        }
        text[kept + delta.size()] = '\0';
    }
};
}  // namespace ui
