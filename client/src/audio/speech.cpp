/** @file speech.cpp
 * @brief 主线程的语句策略：记录句首 → 确认开口 → 逐帧交付 → 确认句尾。
 *
 * 普通开口看连续 VAD；有声回复期间先检测候选，暂停播放后再用参考和人声复核。
 * history 只保存尚未确认的 500ms，开始上传后直接借用当前输入，不保存整句话。
 */
#include "boompi/audio/speech.h"

#include <algorithm>

#include "board_voice_profile.h"
#include "boompi/debug.h"

namespace speech
{
// phase 表示语句处理阶段；各计数器记录当前阶段已经持续的帧数。
enum class Phase
{
    Waiting,    // 等待连续人声，维护句首缓存。
    Probing,    // 已发现插话候选，等待静音观察后确认。
    InSentence  // 已确认一句话，逐帧交付直到句尾。
};
static Phase phase{Phase::Waiting};
static const unsigned kStartFrames = 300 / audio::kFrameMs;
static const unsigned kEndFrames = 700 / audio::kFrameMs;
static const unsigned kMaxFrames = 60000 / audio::kFrameMs;
static const unsigned kCandidateFrames = board_voice::kBargeCandidateMs / audio::kFrameMs;
static const unsigned kProbeFrames = board_voice::kBargeProbeMs / audio::kFrameMs;
static const unsigned kSettleFrames = board_voice::kBargeSettleMs / audio::kFrameMs;
static const unsigned kConfirmFrames = board_voice::kBargeConfirmMs / audio::kFrameMs;
// 前滚保存候选和复核期间的音频；120ms + 380ms 对应完整的 500ms 缓存。
static std::array<audio::VoiceFrame16k, kPreRollFrames> history;
// next 为下一写入位置，stored 为有效帧数；从 next-stored 起按时间顺序回放句首。
static std::size_t next{0}, stored{0};
// voice_frames 是当前阶段连续人声数；utterance_frames 只累计已确认语句的长度。
static unsigned voice_frames{0}, quiet_frames{0}, utterance_frames{0};
static unsigned probe_frames{0}, reference_quiet_frames{0}, retry_frames{0}, tail_frames{0};
// probe_frames限制复核总时长，reference_quiet_frames记录播放安定时长。
// retry_frames是复核失败后的等待，tail_frames覆盖回答停止后的回声衰减。
static bool reference_seen{false};

/** @brief 插话候选需同时满足 VAD 和交流能量；这只是准入条件，后面仍需停播复核。 */
static bool near_voice(const audio::CaptureFrame &frame)
{
    if (!frame.vad_now)
    {
        return false;
    }
    // 交流 RMS 表示语音波动强度；减去均值平方后，以变化的声压作为插话能量依据。
    double sum = 0, squares = 0;
    for (const double sample : frame.pcm)
    {
        sum += sample;
        squares += sample * sample;
    }
    const double kSamples = static_cast<double>(audio::kVoiceFrameSamples);
    const double mean = sum / kSamples;
    // 均方减去均值平方得到交流能量；与RMS门限的平方比较，省去开平方计算。
    return squares / kSamples - mean * mean >=
           board_voice::kBargeMinRms * board_voice::kBargeMinRms;
}

void reset()
{
    next = stored = 0;
    voice_frames = quiet_frames = utterance_frames = 0;
    probe_frames = reference_quiet_frames = retry_frames = tail_frames = 0;
    phase = Phase::Waiting;
    reference_seen = false;
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
        quiet_frames = frame.vad_now ? 0 : quiet_frames + 1;
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
    // 数字参考表明播放链路已有声音；尾音保护覆盖声卡停止后的房间衰减。
    reference_seen = reference_seen || frame.reference_active;
    if (speaking || frame.reference_active)
    {
        tail_frames = board_voice::kPlaybackTailMs / audio::kFrameMs;
    }
    else if (tail_frames != 0)
    {
        --tail_frames;
    }
    // 4. 复核阶段等待播放安定，再用近端人声确认插话；超过复核窗口则恢复回答。
    if (phase == Phase::Probing)
    {
        ++probe_frames;
        // 已写静音和低参考共同表明播放影响正在消退，连续满足条件才累计安定时间。
        if (frame.reference_active || (speaking && !frame.playback_held))
        {
            reference_quiet_frames = 0;
        }
        else
        {
            ++reference_quiet_frames;
        }
        if (reference_quiet_frames > kSettleFrames && near_voice(frame))
        {
            ++voice_frames;
        }
        else
        {
            voice_frames = 0;
        }
        if (voice_frames >= kConfirmFrames)
        {
            // 连续近端人声完成复核，应用将停止当前回答并以这段前滚开始新轮次。
            result.start = true;
            debug::log.barge_confirmed_cb();
        }
        else if (probe_frames >= kProbeFrames)
        {
            // 复核超时清除本次候选，进入短暂重试间隔；应用随后解除暂停并继续原回答。
            phase = Phase::Waiting;
            next = stored = voice_frames = reference_quiet_frames = 0;
            retry_frames = board_voice::kBargeRetryMs / audio::kFrameMs;
            debug::log.barge_rejected_cb(frame.reference_active);
        }
    }
    else if (retry_frames != 0)
    {
        // 重试间隔使一次拒绝结束后有稳定观察时间，随后再检查下一次候选。
        --retry_frames;
        voice_frames = 0;
    }
    else if (tail_frames != 0)
    {
        // 回复或尾音期间：必须见过参考，并有足够长的近端人声，才进入停播复核。
        voice_frames = reference_seen && near_voice(frame) ? voice_frames + 1 : 0;
        if (voice_frames >= kCandidateFrames)
        {
            phase = Phase::Probing;
            probe_frames = reference_quiet_frames = voice_frames = 0;
            debug::log.barge_probe_cb();
        }
    }
    else
    {
        // 普通监听累计连续人声，达到开口门限后确认这句话。
        voice_frames = frame.vad_now ? voice_frames + 1 : 0;
        result.start = voice_frames >= kStartFrames;
    }
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
    // 复核阶段请求暂停消费TTS；确认或拒绝后，应用按start结果选择取消或恢复。
    result.hold_playback = phase == Phase::Probing;
    return result;
}
}  // namespace speech
