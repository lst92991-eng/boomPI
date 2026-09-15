/** @file board_voice_profile.h
 * @brief 当前板卡的内部预置；学生无需校准，应用接口不接收这些参数。
 */
#pragma once

#include <cstdint>

namespace boompi::audio {

namespace board {
// 沿用维护者的双麦极性和Mode1配置；本轮没有据此声称完成当前整板验证。
inline constexpr std::int8_t kLeftMicPolarity = 1;
inline constexpr std::int8_t kRightMicPolarity = 1;
inline constexpr int kAecDelaySamples = 0;

// 三项声学预置不在本轮调整；更换硬件或模型后由维护者整体回归。
inline constexpr char kWakeSensitivity[] = "0.7";
inline constexpr float kSpeechAdmissionDbfs = -30.0F;
inline constexpr float kBargeVoiceDbfs = -25.0F;

static_assert(kAecDelaySamples >= 0 && kAecDelaySamples % 256 == 0,
              "AEC delay must be a non-negative 256-sample multiple");
}  // namespace board

// 同一卡的两个方向独立打开；模型文件由配套部署预置。
inline constexpr char kCapturePcm[] = "hw:0,0";
inline constexpr char kPlaybackPcm[] = "hw:0,0";
inline constexpr char kSnowboyResource[] = "/userdata/boompi/models/common.res";
inline constexpr char kSnowboyModel[] = "/userdata/boompi/models/snowboy.umdl";

}  // namespace boompi::audio
