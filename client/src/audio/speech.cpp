/** @file speech.cpp
 * @brief 主线程的语句策略：记录句首 → 确认开口 → 逐帧交付 → 确认句尾。
 *
 * 监听使用VAD语音活动；播放中插话只累计当前帧命中，句首通过同一份前滚补齐。
 * history 只保存尚未确认的 500ms，开始上传后直接借用当前输入，不保存整句话。
 */
#include "boompi/audio/speech.h"

#include <algorithm>

namespace speech
{
// phase 表示语句处理阶段；各计数器记录当前阶段已经持续的帧数。
enum class Phase
{
    Waiting,    // 等待连续人声，维护句首缓存。
    InSentence  // 已确认一句话，逐帧交付直到句尾。
};
static Phase phase{Phase::Waiting};
static const unsigned kStartFrames = 300 / audio::kFrameMs;
static const unsigned kEndFrames = 700 / audio::kFrameMs;
static const unsigned kMaxFrames = 60000 / audio::kFrameMs;
// 前滚保存确认开口前的500ms音频，当前帧只随前滚交付一次。
static std::array<audio::VoiceFrame16k, kPreRollFrames> history;
// next 为下一写入位置，stored 为有效帧数；从 next-stored 起按时间顺序回放句首。
static std::size_t next{0}, stored{0};
// voice_frames 是当前阶段连续人声数；utterance_frames 只累计已确认语句的长度。
static unsigned voice_frames{0}, quiet_frames{0}, utterance_frames{0};

void reset()
{
    next = stored = 0;
    voice_frames = quiet_frames = utterance_frames = 0;
    phase = Phase::Waiting;
}

Result update(const audio::CaptureFrame &frame, bool speaking)
{
    Result result;
    // 1. 输入断点意味着时间轴中断，清除历史和计数后重新等待一段连续语音。
    if (frame.discontinuity)
    {
        reset();
        return result;
    }
    if (phase == Phase::InSentence)
    {
        // 2. 已确认语句逐帧交付；静音帧也送出，使服务端收到完整句尾。
        quiet_frames = frame.activity != audio::VoiceActivity::Silence ? 0 : quiet_frames + 1;
        result.frames[0] = &frame.pcm;
        result.count = 1;
        ++utterance_frames;
        // 当前帧已经列入返回结果，应用先发送它，再根据end结束上传。
        result.end = utterance_frames >= kMaxFrames || quiet_frames >= kEndFrames;
        return result;
    }
    // 3. 保存开口确认前的音频，确认成功后可补回句首。当前帧也属于这段历史。
    history[next] = frame.pcm;
    next = (next + 1) % kPreRollFrames;
    // 满环后覆盖最早帧，stored维持容量上限；next始终指向下一次写入位置。
    stored = std::min(stored + 1, kPreRollFrames);
    // 4. 播放时只累计当前帧命中；普通监听保留延续，让相邻音节可以连成一句。
    const bool candidate = speaking ? frame.activity == audio::VoiceActivity::Speech
                                    : frame.activity != audio::VoiceActivity::Silence;
    voice_frames = candidate ? voice_frames + 1 : 0;
    result.start = voice_frames >= kStartFrames;
    if (result.start)
    {
        // 5. 确认后按时间顺序交付前滚，随后进入逐帧交付阶段；每个采样只交付一次。
        phase = Phase::InSentence;
        result.count = stored;
        utterance_frames = static_cast<unsigned>(result.count);
        for (std::size_t i = 0; i < result.count; ++i)
        {
            // 从最早有效位置开始取模遍历，返回指针在本次主循环内交付网络。
            result.frames[i] = &history[(next + kPreRollFrames - stored + i) % kPreRollFrames];
        }
    }
    return result;
}
}  // namespace speech
