/** @file board_voice_profile.h
 * @brief 当前板卡的内部预置；学生无需校准，应用接口不接收这些参数。
 */
#pragma once

#include <cstdint>

namespace boompi::audio {

namespace board {
// 极性仅取 +1/-1；AEC 延迟单位为 16k 采样点，当前适配要求非负、按 256 点块对齐。
// 这些是维护者预置，实际接线极性和回采延迟应由板端测量确认。
constexpr std::int8_t kLeftMicPolarity = 1;
constexpr std::int8_t kRightMicPolarity = 1;
constexpr int kAecDelaySamples = 0;

// 唤醒模型灵敏度；语句确认直接使用3A后的WebRTC VAD。
constexpr char kWakeSensitivity[] = "0.7";

// 插话内部预置：保留旧版约-25dBFS的交流RMS准入，不要求学生校准。
constexpr int kBargeMinRms = 1843;
constexpr int kReferencePeak = 64;
constexpr unsigned kBargeCandidateMs = 120;
constexpr unsigned kBargeSettleMs = 160;
constexpr unsigned kBargeConfirmMs = 60;
constexpr unsigned kBargeProbeMs = 380;
constexpr unsigned kBargeRetryMs = 300;
constexpr unsigned kPlaybackTailMs = 300;

}  // namespace board

// 同一卡的两个方向独立打开；模型文件由配套部署预置。
constexpr char kMixerCard[] = "hw:0";
constexpr char kCapturePcm[] = "hw:0,0";
constexpr char kPlaybackPcm[] = "hw:0,0";
constexpr char kSnowboyResource[] = "/userdata/boompi/models/common.res";
constexpr char kSnowboyModel[] = "/userdata/boompi/models/snowboy.umdl";

}  // namespace boompi::audio
