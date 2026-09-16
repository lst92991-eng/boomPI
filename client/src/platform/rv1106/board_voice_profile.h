/** @file board_voice_profile.h
 * @brief 当前板卡的内部预置；学生无需校准，应用接口不接收这些参数。
 */
#pragma once

#include <cstdint>

namespace boompi::audio {

namespace board {
// 极性仅取 +1/-1；AEC 延迟单位为 16k 采样点，当前适配要求非负、按 256 点块对齐。
// 这些是维护者预置，实际接线极性和回采延迟应由板端测量确认。
inline constexpr std::int8_t kLeftMicPolarity = 1;
inline constexpr std::int8_t kRightMicPolarity = 1;
inline constexpr int kAecDelaySamples = 0;

// 唤醒模型灵敏度；语句确认直接使用3A后的WebRTC VAD。
inline constexpr char kWakeSensitivity[] = "0.7";

// 插话内部预置：保留旧版约-25dBFS的交流RMS准入，不要求学生校准。
inline constexpr int kBargeMinRms = 1843;
inline constexpr int kReferencePeak = 64;
inline constexpr unsigned kBargeCandidateMs = 120;
inline constexpr unsigned kBargeSettleMs = 160;
inline constexpr unsigned kBargeConfirmMs = 60;
inline constexpr unsigned kBargeProbeMs = 380;
inline constexpr unsigned kBargeRetryMs = 300;
inline constexpr unsigned kPlaybackTailMs = 300;

}  // namespace board

// 同一卡的两个方向独立打开；模型文件由配套部署预置。
inline constexpr char kMixerCard[] = "hw:0";
inline constexpr char kCapturePcm[] = "hw:0,0";
inline constexpr char kPlaybackPcm[] = "hw:0,0";
inline constexpr char kSnowboyResource[] = "/userdata/boompi/models/common.res";
inline constexpr char kSnowboyModel[] = "/userdata/boompi/models/snowboy.umdl";

}  // namespace boompi::audio
