#pragma once
#include <cstdint>
#include <stdexcept>
#include <string>

#include "audio_vendor.h"

// 只替换厂商分类器；真正的wake初始化、格式检查、异常处理均由产品源执行。
namespace snowboy {
class SnowboyDetect {
 public:
  SnowboyDetect(const std::string&, const std::string&) {}
  void SetSensitivity(const std::string&) {}
  void SetAudioGain(float) {}
  void ApplyFrontend(bool) {}
  int SampleRate() const {
    return 16000;
  }
  int NumChannels() const {
    return 1;
  }
  int BitsPerSample() const {
    return 16;
  }
  int NumHotwords() const {
    return 1;
  }
  bool Reset() {
    ++boompi::test::audio_vendor::wake_resets;
    return true;
  }
  int RunDetection(const std::int16_t*, int, bool) {
    ++boompi::test::audio_vendor::wake_calls;
    if (!boompi::test::audio_vendor::snowboy_process_ok) {
      throw std::runtime_error("injected Snowboy failure");
    }
    return boompi::test::audio_vendor::wake_result;
  }
};
}  // namespace snowboy
