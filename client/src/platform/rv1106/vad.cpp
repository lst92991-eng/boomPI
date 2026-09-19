/** @file vad.cpp
 * @brief WebRTC VAD 薄接口：模式3，保留当前帧命中和语音延续的区别。
 * 当前块的人声结果交给speech.cpp，由连续人声和静音时长确定完整语句及插话。
 */
#include "vad.h"
extern "C"
{
#include <webrtc_vad.h>
#include "webrtc/common_audio/vad/vad_core.h"
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

audio::VoiceActivity process(const audio::VoiceFrame16k &pcm)
{
    if (!detector)
    {
        return audio::VoiceActivity::Error;
    }
    // 固定版内核接口保留1=当前帧命中、>1=延续；公开Process会把两者合并为1。
    // 格式在open核对，实例仍由公开Create/Free管理，内核结构仅限本封装使用。
    const int result = WebRtcVad_CalcVad16khz(
        reinterpret_cast<VadInstT *>(detector), pcm.data(), pcm.size());
    if (result > 1)
    {
        return audio::VoiceActivity::Hangover;
    }
    return result == 1 ? audio::VoiceActivity::Speech : audio::VoiceActivity::Silence;
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
