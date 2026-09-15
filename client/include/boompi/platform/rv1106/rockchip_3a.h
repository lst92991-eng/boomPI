#pragma once
#include "boompi/audio/audio_frames.h"

namespace boompi::rockchip_3a {
// 本模块直接拥有RKAUDIOParam树与vendor句柄；只在采集线程或其启动/退出边界调用。
bool open(int delay_samples = 0) noexcept;
// 当前SDK适配契约为256点块；320点输入/输出通过固定块余数对齐并延迟一帧元数据。
bool process(const audio::CaptureChannels& input, audio::VoiceFrame16k* pcm,
             audio::CaptureMetadata* metadata) noexcept;
void close() noexcept;
}  // namespace boompi::rockchip_3a
