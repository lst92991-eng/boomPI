/** @file board_voice_profile.h
 * @brief 当前板卡的内部预置；学生无需校准，应用接口不接收这些参数。
 */
#pragma once

#include <cstdint>

namespace board_voice
{

// 极性仅取 +1/-1；AEC 延迟单位为 16k 采样点，当前适配要求非负、按 256 点块对齐。
// 麦克极性统一两路波形方向；回采延迟用于对齐参考与麦克信号的采样时间。
const std::int8_t kLeftMicPolarity = 1;
const std::int8_t kRightMicPolarity = 1;
const int kAecDelaySamples = 0;

// 唤醒模型灵敏度；语句确认直接使用3A后的WebRTC VAD。
const char kWakeSensitivity[] = "0.7";

// 插话准入使用约 -25dBFS 的交流 RMS；位置和音量变化后需要实际对话确认。
const int kBargeMinRms = 1843;
const int kReferencePeak = 64;
const unsigned kBargeCandidateMs = 120;
const unsigned kBargeSettleMs = 160;
const unsigned kBargeConfirmMs = 60;
const unsigned kBargeProbeMs = 380;
const unsigned kBargeRetryMs = 300;
const unsigned kPlaybackTailMs = 300;

// 同一卡的两个方向独立打开；模型文件由配套部署预置。
const char kMixerCard[] = "hw:0";
const char kCapturePcm[] = "hw:0,0";
const char kPlaybackPcm[] = "hw:0,0";
const char kSnowboyResource[] = "/userdata/boompi/models/common.res";
const char kSnowboyModel[] = "/userdata/boompi/models/snowboy.umdl";

}  // namespace board_voice
