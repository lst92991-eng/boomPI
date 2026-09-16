/** @file vad.cpp
 * @brief WebRTC VAD 薄接口：配置模式 3，逐块返回错误/无人声/有人声。
 * 当前块的人声结果交给speech.cpp，由连续人声和静音时长确定完整语句及插话。
 */
#include "vad.h"
extern "C"
{
#include <webrtc_vad.h>
}

namespace vad
{
static VadInst *detector{};

bool reset()
{
    return detector && WebRtcVad_Init(detector) == 0 && WebRtcVad_set_mode(detector, 3) == 0;
}

bool open()
{
    if (detector)
    {
        return false;
    }
    detector = WebRtcVad_Create();
    if (!reset() || WebRtcVad_ValidRateAndFrameLength(16000, audio::kVoiceFrameSamples) != 0)
    {
        close();
        return false;
    }
    return true;
}

int process(const audio::VoiceFrame16k &pcm)
{
    return detector ? WebRtcVad_Process(detector, 16000, pcm.data(), pcm.size()) : -1;
}

void close()
{
    if (detector)
    {
        WebRtcVad_Free(detector);
        detector = nullptr;
    }
}
}  // namespace vad
