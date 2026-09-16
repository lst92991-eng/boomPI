/** @file rockchip_3a.cpp
 * @brief 配置并调用一个 Rockchip 声学处理句柄，将双麦/单参考转换为单声道。
 *
 * open 依次填写 AEC、AES、ANR、去混响和双讲参数，再建立统一的厂商处理句柄。
 * process 只适配当前 256 点厂商块和 320 点业务帧，用户插话策略在 speech.cpp。
 */
#include "boompi/platform/rv1106/rockchip_3a.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <new>

#include "board_voice_profile.h"
#include "rkaudio_preprocess.h"

namespace rockchip_3a
{

// 保留当前适配器的16kHz/双麦/单参考契约，修改支持范围需匹配SDK与真板证据。
// read_size取256个采样时刻，业务每次交付320个采样时刻，块余数在本模块保存。
static const int kVendorBlockSamples = 256;
static const std::size_t kVendorInputChannels = 3U;
// 当前厂商块 256 <= 业务帧 320，配合一帧预填，输出容量取两帧；改块长须重新核算余数。
static const std::size_t kOutputFifoSamples = 2 * audio::kVoiceFrameSamples;
static void *handle{nullptr};
static RKAUDIOParam *parameters{nullptr};
static std::array<std::int16_t, kVendorBlockSamples * kVendorInputChannels> input_block{};
static std::array<std::int16_t, kOutputFifoSamples> output_fifo{};
// input_shorts 数交错 S16 元素（包含三个通道）；output_count 数单声道采样点。
static std::size_t input_shorts{0U}, output_count{0U};

static const int kVendorInputShorts = kVendorBlockSamples * kVendorInputChannels;
static const int kVendorOutputBytes =
    kVendorBlockSamples * static_cast<int>(sizeof(std::int16_t));
static const int kMainFeatureMask = RKAUDIO_EN_AEC | RKAUDIO_EN_BF;
// BF 参数树承载 FastAEC、残余回声抑制、降噪、去混响和双讲保护的组合开关。
static const int kBeamformingFeatureMask =
    EN_Fastaec | EN_AES | EN_Anr | EN_Dereverberation | EN_STDT;

// 直接使用配套 SDK 的头文件和库；当前 ARM 工具链的 short 为 16 位。
// SDK头文件和动态库共同确定参数树、返回长度与功能位，升级时需要成套核对。

bool open()
{
    // 1. 根参数树保存各声学功能的配置；零初始化使失败清理能识别已取得的子结构。
    parameters = new (std::nothrow) RKAUDIOParam{};
    if (!parameters)
    {
        return false;
    }
    parameters->model_en = kMainFeatureMask;
    parameters->read_size = kVendorBlockSamples;
    parameters->aec_param = rkaudio_aec_param_init();
    parameters->bf_param = rkaudio_preprocess_param_init();
    // 2. 厂商函数建立AEC和前处理子树，随后逐组填写本板使用的参数。
    auto *aec = static_cast<SKVAECParameter *>(parameters->aec_param);
    auto *bf = static_cast<SKVPreprocessParam *>(parameters->bf_param);
    if (!aec || !bf || !aec->delay_para || !bf->aes_para || !bf->anr_para ||
        !bf->dereverb_para || !bf->dtd_para)
    {
        close();
        return false;
    }
    // AEC：输入双麦与一个数字回采参考，延迟使用板级配置，供算法估计扬声器回声。
    aec->pos = 1;
    aec->model_aec_en = 0;
    aec->drop_ref_channel = 0;
    aec->delay_len = board_voice::kAecDelaySamples;
    bf->model_bf_en = kBeamformingFeatureMask;
    bf->Targ = 4;
    bf->ref_pos = 1;
    bf->num_ref_channel = 1;
    bf->drop_ref_channel = 0;
    // AES：残余回声抑制。
    auto *aes = static_cast<RKAudioAESParameter *>(bf->aes_para);
    aes->Beta_Up_Low = 0.005F;
    aes->THD_Flag = 0;
    aes->HARD_Flag = 0;
    // ANR：用噪声估计参数与最低增益控制背景噪声抑制强度。
    auto *anr = static_cast<SKVANRParam *>(bf->anr_para);
    anr->swU = 1;
    anr->fGmin = 0.01F;
    anr->InterV = 1;
    // 去混响：保留当前房间衰减预置。
    auto *dereverb = static_cast<RKAudioDereverbParam *>(bf->dereverb_para);
    dereverb->curveLg = 20;
    dereverb->T60 = 0.4F;
    // 双讲保护：配置近端和远端同时说话时的处理门限；业务插话由speech模块判断。
    auto *dtd = static_cast<RKDTDParam *>(bf->dtd_para);
    dtd->ksiThd_high = 0.70F;
    dtd->ksiThd_low = 0.50F;
    // 参数树准备完整后交给统一句柄；后续每个厂商块通过同一个process接口处理。
    handle = rkaudio_preprocess_init(16000, 16, 2, 1, parameters);
    if (!handle)
    {
        close();
        return false;
    }
    // 3. 预填一帧静音，使256点算法块能连续按320点业务帧交付。
    input_shorts = 0;
    output_count = audio::kVoiceFrameSamples;
    output_fifo.fill(0);
    return true;
}

void close()
{
    // 处理句柄会使用参数树，先销毁句柄再释放参数树及其子结构。
    if (handle)
    {
        rkaudio_preprocess_destory(handle);
        handle = nullptr;
    }
    if (parameters)
    {
        rkaudio_param_deinit(parameters);
        delete parameters;
        parameters = nullptr;
    }
}

bool process(const audio::CaptureChannels &input, audio::VoiceFrame16k &pcm)
{
    // 输入按mic0/mic1/refL交错排列；填满一个厂商块再处理，跨业务帧保留尚欠的样本。
    for (std::size_t offset = 0; offset < input.size();)
    {
        const auto count = std::min(input.size() - offset, input_block.size() - input_shorts);
        std::copy_n(input.data() + offset, count, input_block.data() + input_shorts);
        offset += count;
        input_shorts += count;
        if (input_shorts < input_block.size())
        {
            break;
        }
        // 上次剩余输出+未成块输入共320点；本次再输入320，输出永远不超过640。
        int wakeup_status = 0;
        // 输入长度按S16元素计，三通道共768个元素；正常输出为256点单声道，共512字节。
        const int result = rkaudio_preprocess_short(
            handle, reinterpret_cast<short *>(input_block.data()),
            reinterpret_cast<short *>(output_fifo.data() + output_count), kVendorInputShorts,
            &wakeup_status);
        if (result != kVendorOutputBytes)
        {
            // vendor 返回长度或 FIFO 不变量失效后，继续处理会错位整个音频时间轴。
            close();
            return false;
        }
        input_shorts = 0U;
        output_count += kVendorBlockSamples;
    }
    // 每次交付 320 点；预填静音带来固定一帧的适配延迟，厂商库内部延迟需另行测量。
    std::copy_n(output_fifo.data(), pcm.size(), pcm.data());
    output_count -= pcm.size();
    std::memmove(output_fifo.data(), output_fifo.data() + pcm.size(),
                 output_count * sizeof(std::int16_t));
    return true;
}

}  // namespace rockchip_3a
