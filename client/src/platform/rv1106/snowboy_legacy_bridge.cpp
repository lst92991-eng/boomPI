/**
 * @file snowboy_legacy_bridge.cpp
 * @brief 把 Snowboy 旧 C++ ABI 封装成主客户端可安全调用的最小 C 接口。
 *
 * 只有本翻译单元包含 Snowboy C++ 类型和异常行为；跨模块边界仅传递 POD、PCM
 * 指针与不透明句柄，使旧 ABI 编译选项保持局部生效。
 * SpeechDetector 的 Detect 每 20 ms 调用 process；返回值表示这次接口调用是否完成，
 * detection_result 才表示是否命中唤醒词，两种结果必须分别判断。
 */
#include "snowboy_legacy_bridge.h"

#include <climits>
#include <memory>
#include <string>

#include "snowboy-detect.h"

// 只有本翻译单元包含 Snowboy C++ 类型，并按旧 libstdc++ ABI 单独编译。
// 主程序只保存 BoompiSnowboyLegacyHandle 指针，不解析 detector 的对象布局。
struct BoompiSnowboyLegacyHandle final {
  snowboy::SnowboyDetect* detector;
};

/** @brief 在旧 ABI 单元内构造模型并校验元数据，成功后把销毁责任交给 destroy。 */
int boompi_snowboy_legacy_create(const char* resource, const char* model,
                                 const char* sensitivity, float gain,
                                 BoompiSnowboyLegacyHandle** handle) {
  // 参数检查失败不会写输出；调用方把句柄初始化为空，通过检查后才开始资源创建。
  if (resource == nullptr || *resource == '\0' || model == nullptr || *model == '\0' ||
      sensitivity == nullptr || *sensitivity == '\0' || handle == nullptr) {
    return 0;
  }
  *handle = nullptr;
  try {
    auto detector = std::make_unique<snowboy::SnowboyDetect>(resource, model);
    // Rockchip 3A 已完成前端处理，因此关闭 Snowboy 自带 frontend，避免二次处理改变模型输入。
    detector->SetSensitivity(sensitivity);
    detector->SetAudioGain(gain);
    detector->ApplyFrontend(false);
    // 产品帧契约固定为 16 kHz/S16/mono；模型元数据不匹配时在启动阶段拒绝运行。
    if (detector->SampleRate() != 16000 || detector->NumChannels() != 1 ||
        detector->BitsPerSample() != 16 || detector->NumHotwords() < 1) {
      return 0;
    }
    *handle = new BoompiSnowboyLegacyHandle{detector.release()};
    return 1;
  } catch (...) {
    return 0;
  }
}

/** @brief 在创建对象的 ABI 环境中销毁模型；退出前调用方须确保不再并发 reset/process。 */
void boompi_snowboy_legacy_destroy(BoompiSnowboyLegacyHandle* handle) {
  if (handle == nullptr) {
    return;
  }
  // C ABI 不能传播 C++ 异常；销毁失败仍需释放外层 handle，进程退出路径保持可终止。
  try {
    delete handle->detector;
  } catch (...) {
  }
  delete handle;
}

/** @brief 清除模型内部跨帧历史，避免上一监听阶段的声音触发下一阶段唤醒。 */
int boompi_snowboy_legacy_reset(BoompiSnowboyLegacyHandle* handle) {
  if (handle == nullptr || handle->detector == nullptr) {
    return 0;
  }
  try {
    return handle->detector->Reset() ? 1 : 0;
  } catch (...) {
    return 0;
  }
}

/** @brief 仅借用本次 PCM 缓冲；异常转为返回失败，由 SpeechDetector 结束当前处理链。 */
int boompi_snowboy_legacy_process_s16(BoompiSnowboyLegacyHandle* handle, const int16_t* samples,
                                      uint32_t count, int32_t* result) {
  // INT_MAX 检查保护 Snowboy 的 int 长度参数；调用方通常传入固定 320 samples。
  if (handle == nullptr || handle->detector == nullptr || samples == nullptr || count == 0U ||
      count > static_cast<uint32_t>(INT_MAX) || result == nullptr) {
    return 0;
  }
  try {
    // RunDetection > 0 表示命中某个热词，0 表示未命中，负值由上层按 Snowboy 结果处理。
    *result = handle->detector->RunDetection(samples, static_cast<int>(count), false);
    return 1;
  } catch (...) {
    *result = 0;
    return 0;
  }
}
