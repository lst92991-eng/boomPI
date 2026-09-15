#pragma once
#include <cstdint>
namespace boompi::audio_capture {
// 只管理原始四槽PCM；0成功、负值为ALSA错误码。
int open() noexcept;
// 固定20ms：正值为完整帧数，0为断流，-ECANCELED为主动停止，其余负值为错误。
int read(std::int16_t* pcm) noexcept;
// 中断阻塞读取，所属任务退出后再close。
int interrupt() noexcept;
void close() noexcept;
}  // namespace boompi::audio_capture
