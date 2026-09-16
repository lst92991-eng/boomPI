/** @file wake.cpp
 * @brief Snowboy 唤醒检测；本文件同时承担旧 C++ ABI 隔离，不另设桥接转发层。
 *
 * 构造、检测和复位可能抛第三方异常，必须在本 ABI 内捕获，再以 bool/整数交付。
 * 运行期只有采集线程访问 detector；这里不判断语句开始、结束或插话。
 */
#include "wake.h"

#include <memory>

#include "board_voice_profile.h"
#include "snowboy-detect.h"

namespace boompi::wake {
namespace {
// 只有本文件按旧C++ ABI编译；公开接口不传string或Snowboy对象。
std::unique_ptr<snowboy::SnowboyDetect> detector;
}  // namespace
bool open() noexcept {
  try {
    detector =
        std::make_unique<snowboy::SnowboyDetect>(audio::kSnowboyResource, audio::kSnowboyModel);
    detector->SetSensitivity(audio::board::kWakeSensitivity);
    detector->SetAudioGain(1.0F);
    // 3A已经处理音频，关闭Snowboy自带前端，保留匹配模型的输入格式检查。
    detector->ApplyFrontend(false);
    if (detector->SampleRate() == 16000 && detector->NumChannels() == 1 &&
        detector->BitsPerSample() == 16 && detector->NumHotwords() > 0) {
      return true;
    }
  } catch (...) {
    // 第三方异常在当前ABI内收住，调用方报告初始化阶段。
  }
  close();
  return false;
}

int detect(const audio::VoiceFrame16k& pcm) noexcept {
  try {
    const int result = detector->RunDetection(pcm.data(), static_cast<int>(pcm.size()), false);
    // SDK的-2是静音、-1是错误；模块只向主线交付错误/未命中/命中。
    if (result == -2) {
      return 0;
    }
    if (result < 0) {
      return -1;
    }
    return result > 0 ? 1 : 0;
  } catch (...) {
    return -1;
  }
}

bool reset() noexcept {
  try {
    return detector->Reset();
  } catch (...) {
    return false;
  }
}

void close() noexcept {
  detector.reset();
}
}  // namespace boompi::wake
