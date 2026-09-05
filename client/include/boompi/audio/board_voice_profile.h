#pragma once

/**
 * @file board_voice_profile.h
 * @brief RV1106 板卡的采样格式和声学标定参数。
 *
 * 更换麦克风、扬声器或模型后，需要重新验证整组参数。参数在此固定，启动时
 * 不再从环境变量或编译选项覆盖，以免同一份源码产生不同的声学行为。
 */

#include <cstdint>

#include "boompi/audio/audio_format.h"

namespace boompi::audio {

struct BoardVoiceProfile final {
  std::int8_t left_polarity;
  std::int8_t right_polarity;
  const char* wake_sensitivity;
  float vad_admission_dbfs;
  float barge_voice_dbfs;
  int aec_delay_samples;
};

// Codec Mode1 的麦克风和数字参考同步采集，不额外插入软件延迟。
inline constexpr BoardVoiceProfile kBoardVoiceProfile{1, 1, "0.7", -30.0F, -25.0F, 0};

static_assert(kBoardVoiceProfile.aec_delay_samples >= 0 &&
                  kBoardVoiceProfile.aec_delay_samples % 256 == 0,
              "Rockchip AEC delay must be a non-negative 256-sample multiple");

inline constexpr char kCapturePcm[] = "hw:0,0";
inline constexpr char kPlaybackPcm[] = "hw:0,0";
inline constexpr char kSnowboyResource[] = "/userdata/boompi/models/common.res";
inline constexpr char kSnowboyModel[] = "/userdata/boompi/models/snowboy.umdl";

}  // namespace boompi::audio
